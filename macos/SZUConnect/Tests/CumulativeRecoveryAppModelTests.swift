import Foundation
import Darwin

/// Exercise the real model. Replies and completion order are entirely in-memory;
/// no application, socket, system preferences, permission request or Keychain.
private actor CumulativeReplies {
    private var values: [ConnectivityReport] = []
    private var paused = false
    private var continuation: CheckedContinuation<Void, Never>?
    private(set) var calls = 0
    func enqueue(_ values: [ConnectivityReport], pause: Bool = false) {
        self.values = values; paused = pause
    }
    func next() async -> ConnectivityReport {
        calls += 1
        let value = values.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : values.removeFirst()
        if paused {
            paused = false
            // Ignore task cancellation deliberately: the model must reject late replies.
            await withCheckedContinuation { continuation = $0 }
        }
        return value
    }
    var waiting: Bool { continuation != nil }
    func release() { continuation?.resume(); continuation = nil }
}

private actor CumulativeService: ConnectionService {
    let replies: CumulativeReplies
    private var route: CampusPhysicalRoute?
    private var discoveries = 0
    private var attempts = 0
    private var probeRoutes: [String?] = []
    private var discovery = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed, teachingClientBound: true)
    init(_ replies: CumulativeReplies) { self.replies = replies }
    func setDiscovery(_ value: CampusDiscovery) { discovery = value }
    func setExpectedPhysicalRoute(_ value: CampusPhysicalRoute?) -> Bool { route = value; return true }
    func resetConnections() { route = nil }
    func probe() async -> ConnectivityReport {
        probeRoutes.append(route?.identity)
        return await replies.next()
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries += 1
        return discovery
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        attempts += 1
        return LoginAttempt(outcome: .unavailable)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { nil }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { throw PortalError.network }
    func snapshot() -> (Int, Int, [String?]) { (discoveries, attempts, probeRoutes) }
}

private enum CumulativeWaitFault: Error { case injected }

/// Both failure stages enter through the real physical transport and PortalClient.
/// Only the associated resolver, encrypted resolver and final socket are fake.
private final class CumulativeTLSPhases: @unchecked Sendable {
    enum Phase { case dnsTLS, campusTLS, healthy }
    private let lock = NSLock()
    private var phase: Phase = .dnsTLS
    private var associatedCalls = 0, encryptedCalls = 0, targetCalls = 0
    func use(_ value: Phase) { lock.withLock { phase = value } }
    func associated() throws -> [String] {
        let value = lock.withLock { associatedCalls += 1; return phase }
        if value == .dnsTLS { throw CampusPhysicalTransportError.dns }
        return ["192.0.2.80"]
    }
    func encrypted() throws -> CampusPhysicalAddressResolver.Answer {
        lock.withLock { encryptedCalls += 1 }
        throw CampusPhysicalTransportError.tls
    }
    func target(_ url: URL) throws -> CampusPhysicalTransferResult {
        let value = lock.withLock { targetCalls += 1; return phase }
        precondition(url.path == "/srun_portal_pc" && url.query == nil, "TLS fixture permits only credential-free discovery")
        if value == .campusTLS { throw CampusPhysicalTransportError.tls }
        let page = "<html><title>Shenzhen University srun login</title><script>var CONFIG={acid:5,ip:'192.0.2.42'};</script></html>"
        return .init(data: Data(page.utf8), headers: "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n", status: 200)
    }
    var counts: (Int, Int, Int) { lock.withLock { (associatedCalls, encryptedCalls, targetCalls) } }
}
private actor CumulativeWaits {
    private var regularCalls = 0
    private var fallbackValues: [TimeInterval] = []
    private var pending: [UUID: CheckedContinuation<Void, Error>] = [:]
    func regular(_ seconds: Int) -> Bool { regularCalls += 1; return regularCalls == 1 }
    func fallback(_ seconds: TimeInterval) async throws {
        fallbackValues.append(seconds)
        try await wait()
    }
    func wait() async throws {
        let id = UUID()
        try Task.checkCancellation()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { pending[id] = $0 }
        } onCancel: { Task { await self.cancel(id) } }
        try Task.checkCancellation()
    }
    private func cancel(_ id: UUID) { pending.removeValue(forKey: id)?.resume(throwing: CancellationError()) }
    func release() { let values = pending.values; pending.removeAll(); for value in values { value.resume() } }
    func snapshot() -> (Int, [TimeInterval], Int) { (regularCalls, fallbackValues, pending.count) }
}

@MainActor private final class CumulativeClock {
    // Anchor the injected wall clock to worker completion dates; relative time remains controlled.
    var date = Date()
    var uptime: TimeInterval = 100
}

@main @MainActor
struct CumulativeRecoveryAppModelTests {
    private static var assertions = 0
    private static var failures: [String] = []
    private nonisolated static let online = ConnectivityReport(apple: true, microsoft: true)
    private nonisolated static let offline = ConnectivityReport(apple: false, microsoft: false)
    private nonisolated static let incomplete = ConnectivityReport(appleResult: .init(state: .timeout, elapsed: 1),
        microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .complete)
    private nonisolated static let cancelled = ConnectivityReport(appleResult: .init(state: .cancelled, elapsed: 0),
        microsoftResult: .init(state: .timeout, elapsed: 1), mode: .complete)
    private static let descriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "02:00:00:00:cc:01", "gwip": "192.0.2.1", "net": "192.0.2.0/24",
        "dhcp": "192.0.2.2", "dns": "192.0.2.3"
    ])
    private static func check(_ value: Bool, _ text: String) {
        assertions += 1
        if value { print("PASS: \(text)") }
        else { failures.append(text); print("FAIL: \(text)") }
    }
    private static func wait(_ text: String, _ predicate: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(8))
        while ContinuousClock.now < deadline {
            if await predicate() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(text)")
    }
    private static func route(_ lease: String = "A") -> CampusPhysicalRoute {
        CampusPhysicalRoute(interfaceName: "en42", interfaceIndex: 42, addresses: ["192.0.2.42"],
            dnsServers: ["192.0.2.3"], identity: "cumulative-fixture-route-\(lease)",
            serviceID: "cumulative-fixture-service", descriptor: descriptor, isSystemPrimary: true)
    }
    private static func attach(_ model: AppModel, lease: String = "A", tunnel: Bool = true,
                               identified: Bool = true, connected: Bool = true) {
        model.receivePath(usable: connected, interfaces: connected ? "有线 en42" : "", identity:
            identified ? NetworkIdentity(fingerprint: "cumulative-fixture-network") : nil,
            linkActive: connected, tunnel: tunnel, descriptors: identified ? [descriptor] : [],
            physicalConnected: connected, systemPhysicalLabel: "有线 en42",
            connectedPhysicalLabels: connected ? ["有线 en42"] : [],
            authenticationRoute: identified && connected ? route(lease) : nil, requiresPhysicalBinding: true)
    }
    private static func fixture(identified: Bool = true, tunnel: Bool = true, readOnly: Bool = false,
                                remembered: Bool = false, credentials: Bool = false,
                                clientOverride: (any ConnectionService)? = nil) throws -> (AppModel, CumulativeService, CumulativeReplies, CumulativeReplies, CumulativeClock, URL) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-cumulative-model-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let clock = CumulativeClock()
        if remembered {
            var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
            _ = memory.remember(descriptor, area: "teaching", proofTeaching: true, proofDormitory: false,
                now: clock.date.addingTimeInterval(-1_000), newID: "01ab23cd")
            try NetworkMemoryFile.write(memory.encoded(), to: root.appendingPathComponent("memory.json"))
        }
        let base = CumulativeReplies(), system = CumulativeReplies(), service = CumulativeService(base)
        let model = AppModel(client: clientOverride ?? service, isolated: true, readOnly: readOnly,
            now: { clock.date }, monotonicNow: { clock.uptime },
            recoveryStore: root.appendingPathComponent("recovery.json"),
            isolatedCredentials: credentials ? Credentials(account: "cumulative-fixture-account", password: "cumulative-fixture-password") : nil,
            memoryStore: root.appendingPathComponent("memory.json"),
            systemConnectivityProbe: { _ in await system.next() }, verificationSleep: { _ in })
        model.area = .teaching
        model.setMonitoringMode(.observe)
        model.setProbeMode(.complete)
        attach(model, tunnel: tunnel, identified: identified)
        return (model, service, base, system, clock, root)
    }
    private static func round(_ model: AppModel, _ base: CumulativeReplies, _ system: CumulativeReplies,
                              physical: ConnectivityReport = offline, current: ConnectivityReport = offline) async throws {
        await base.enqueue([physical]); await system.enqueue([current])
        await model.runAutomaticCycle()
        try await wait("system round completion") { !model.systemConnectivityChecking }
    }
    private static func failTool(_ model: AppModel) {
        let token = model.beginCampusDirectToolCheck()
        model.finishCampusDirectToolCheck(token, campusPassed: false, cancelled: false)
    }
    static func main() async throws {
        setbuf(stdout, nil)

        do {
            let (model, service, base, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            try await round(model, base, system)
            check(!model.campusDirectHelpSuggested, "One genuinely completed pair is below the offline-help threshold")
            // Merely observing the completed system result again cannot count a new round.
            attach(model); attach(model)
            check(!model.campusDirectHelpSuggested, "Repeated same-path notifications do not count completed results twice")
            try await round(model, base, system)
            check(model.campusDirectHelpSuggested, "Two separately completed failed fixed-base/system pairs suggest offline help")
            let snapshot = await service.snapshot()
            check(!model.currentNetworkAuthorized && snapshot.1 == 0,
                  "Failure recommendations neither grant trust nor submit credentials")
        }
        for uncounted in [incomplete, cancelled] {
            let (model, _, base, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            try await round(model, base, system, physical: uncounted)
            try await round(model, base, system, physical: uncounted)
            check(!model.campusDirectHelpSuggested, "Incomplete/cancelled physical fixed probes cannot count as failed pairs")
            try await round(model, base, system, current: uncounted)
            try await round(model, base, system, current: uncounted)
            check(!model.campusDirectHelpSuggested, "Incomplete/cancelled system fixed probes cannot count as failed pairs")
        }
        do {
            let (model, _, _, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            for _ in 0..<3 {
                await system.enqueue([offline]); model.refreshSystemConnectivity()
                try await wait("unpaired system completion") { !model.systemConnectivityChecking }
            }
            check(!model.campusDirectHelpSuggested, "Known physical route requires its own completed fixed-base results")
        }
        do {
            let (model, service, base, system, _, root) = try fixture(identified: false)
            defer { try? FileManager.default.removeItem(at: root) }
            // No physical probe is executed; only completed system observations exist.
            for index in 0..<2 {
                await system.enqueue([offline]); model.refreshSystemConnectivity()
                try await wait("unknown-route system completion") { !model.systemConnectivityChecking }
                check(model.campusDirectHelpSuggested == (index == 1), "Unknown physical route counts only actual completed system failures")
            }
            let physicalCalls = await base.calls, snapshot = await service.snapshot()
            check(physicalCalls == 0 && snapshot.1 == 0 && !model.currentNetworkAuthorized,
                  "System-only help never invents physical measurements or campus authorization")
        }
        do {
            let (model, _, base, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            failTool(model)
            check(!model.campusDirectHelpSuggested, "One campus-tool failure is below its independent threshold")
            try await round(model, base, system, physical: online, current: online)
            failTool(model)
            check(model.campusDirectHelpSuggested, "System/base success preserves the separate campus-tool failure sequence")
            let beforeSuccess = model.beginCampusDirectToolCheck()
            let successful = model.beginCampusDirectToolCheck()
            model.finishCampusDirectToolCheck(successful, campusPassed: true, cancelled: false)
            check(!model.campusDirectHelpSuggested, "Fresh campus-tool success clears its own failure sequence")
            model.finishCampusDirectToolCheck(beforeSuccess, campusPassed: false, cancelled: false)
            failTool(model)
            check(!model.campusDirectHelpSuggested, "Failure started before campus success cannot become a later first failure")
            failTool(model)
            check(model.campusDirectHelpSuggested, "Two genuinely new campus-tool failures can suggest help again")
        }
        do {
            let (model, _, base, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            try await round(model, base, system); try await round(model, base, system)
            let successful = model.beginCampusDirectToolCheck()
            model.finishCampusDirectToolCheck(successful, campusPassed: true, cancelled: false)
            check(model.campusDirectHelpSuggested, "Campus-tool success cannot erase the independent failed-network source")
            try await round(model, base, system, physical: online, current: online)
            check(!model.campusDirectHelpSuggested, "Network success clears its own failure source")
        }
        do {
            let (model, service, base, system, _, root) = try fixture(remembered: true, credentials: true)
            defer { try? FileManager.default.removeItem(at: root) }
            model.setMonitoringMode(.recover)
            failTool(model); failTool(model)
            let late = model.beginCampusDirectToolCheck()
            check(model.campusDirectHelpSuggested, "Two campus-tool failures are initially visible before a recovery round")
            try await round(model, base, system, current: online)
            try await round(model, base, system, current: online)
            let snapshot = await service.snapshot()
            check(snapshot.0 == 1 && snapshot.1 == 1 && !model.campusDirectHelpSuggested,
                  "Fresh applicable recovery discovery clears old campus failures even when its one simulated login fails")
            model.finishCampusDirectToolCheck(late, campusPassed: false, cancelled: false)
            failTool(model)
            check(!model.campusDirectHelpSuggested,
                  "Tool failure started before recovery discovery success cannot revive the obsolete failure sequence")
        }
        do {
            let (model, service, base, system, _, root) = try fixture(remembered: true, credentials: true)
            defer { try? FileManager.default.removeItem(at: root) }
            model.area = .automatic; model.setMonitoringMode(.recover)
            failTool(model); failTool(model)
            let conflict = ConnectivityReport(appleResult: .init(state: .redirect, elapsed: 0.1, campusAreaHint: .teaching),
                microsoftResult: .init(state: .redirect, elapsed: 0.1, campusAreaHint: .dormitory), mode: .complete)
            try await round(model, base, system, physical: conflict, current: online)
            try await round(model, base, system, physical: conflict, current: online)
            let snapshot = await service.snapshot()
            check(snapshot.0 == 1 && snapshot.1 == 0 && model.campusDirectHelpSuggested,
                  "Conflicting fresh recovery hints cannot clear campus failures merely because both homepages respond")
        }
        do {
            let (model, _, _, _, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            let token = model.beginCampusDirectToolCheck()
            model.finishCampusDirectToolCheck(token, campusPassed: false, cancelled: false)
            model.finishCampusDirectToolCheck(token, campusPassed: false, cancelled: false)
            check(!model.campusDirectHelpSuggested, "A completed tool token can be consumed only once")
            failTool(model)
            check(model.campusDirectHelpSuggested, "A second unique tool operation reaches the two-failure threshold")
        }
        for action in ["cancel", "path", "tun", "sleep", "forget"] {
            let (model, _, _, _, _, root) = try fixture(remembered: true)
            defer { try? FileManager.default.removeItem(at: root) }
            failTool(model)
            let late = model.beginCampusDirectToolCheck()
            switch action {
            case "path": attach(model, lease: "B")
            case "tun": attach(model, tunnel: false); attach(model)
            case "sleep": model.setSleeping(true); model.setSleeping(false); attach(model)
            case "forget": model.forgetCurrentNetwork()
            default: model.cancelCampusDirectToolChecks()
            }
            model.finishCampusDirectToolCheck(late, campusPassed: false, cancelled: false)
            check(!model.campusDirectHelpSuggested, "\(action) clears tool history and rejects old completion")
            failTool(model)
            check(!model.campusDirectHelpSuggested, "\(action) leaves a genuinely new first tool failure below threshold")
        }
        for kind in ["readOnly", "noTun", "noPhysical"] {
            let (model, _, _, _, _, root) = try fixture(tunnel: kind != "noTun", readOnly: kind == "readOnly")
            defer { try? FileManager.default.removeItem(at: root) }
            if kind == "noPhysical" { attach(model, connected: false) }
            failTool(model); failTool(model)
            check(!model.campusDirectHelpSuggested, "\(kind) cannot produce an offline-help recommendation from tool results")
        }
        for change in ["path", "forget", "area", "cancel"] {
            let (model, service, base, _, _, root) = try fixture(remembered: true)
            defer { try? FileManager.default.removeItem(at: root) }
            model.area = .automatic
            await base.enqueue([online], pause: true)
            model.verifyCurrentCampusService()
            try await wait("private current hint sampling") { await base.waiting }
            check(model.busy && model.campusVerificationInProgress,
                  "Independent verification owns its private quick probe before discovery")
            switch change {
            case "path": attach(model, lease: "B")
            case "forget": model.forgetCurrentNetwork()
            case "area": model.area = .dormitory
            default: model.cancelCurrentOperation()
            }
            let feedback = model.memoryVerificationMessage
            await base.release()
            try await wait("private quick worker cancelled") { !model.busy }
            // Main-actor cancellation is observed after the fake has resumed.
            for _ in 0..<10 { await Task.yield() }
            let snapshot = await service.snapshot()
            check(snapshot.0 == 0 && snapshot.1 == 0,
                  "\(change) after private quick sampling prevents discovery and any credential submission")
            check(snapshot.2 == [route().identity] && model.requiredCampusVerificationState != "confirmed"
                  && model.memoryVerificationMessage == feedback,
                  "\(change) rejects the old physical binding and cannot install late private proof")
        }
        do {
            let (model, service, base, system, _, root) = try fixture(remembered: true)
            defer { try? FileManager.default.removeItem(at: root) }
            try await round(model, base, system, physical: online, current: online)
            let visible = model.lastInternetCheck!.base
            let checkedAt = model.lastCheckedAt
            model.area = .automatic
            let fresh = ConnectivityReport(appleResult: .init(state: .redirect, elapsed: 0.1,
                campusAreaHint: .teaching), microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick)
            await base.enqueue([fresh])
            model.verifyCurrentCampusService()
            try await wait("successful private hints") { !model.busy }
            check(model.requiredCampusVerificationState == "confirmed"
                  && model.currentCampusRouting == .selected(.teaching, allowUnsentFallback: false),
                  "A new private redirect hint can select and independently confirm the current teaching service")
            check(model.lastCheckedAt == checkedAt && model.lastInternetCheck?.base.summary == visible.summary
                  && model.lastInternetCheck?.base.mode == visible.mode,
                  "Private quick hints preserve the visible report, mode and check timestamp")
            let snapshot = await service.snapshot()
            check(snapshot.2 == [route().identity, route().identity] && snapshot.1 == 0 && !model.currentNetworkAuthorized,
                  "Private hints use the same bound route and never turn service evidence into trust")
        }
        for cancelFallback in [false, true] {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-monitor-wait-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: root) }
            let base = CumulativeReplies(), service = CumulativeService(base), clock = CumulativeClock(), waits = CumulativeWaits()
            await base.enqueue([online])
            let model = AppModel(client: service, isolated: true, now: { clock.date },
                monotonicNow: { clock.uptime }, recoveryStore: root.appendingPathComponent("recovery.json"),
                runAutomaticLoopInIsolation: true, loopSleep: { seconds in
                    if await waits.regular(seconds) {
                        await MainActor.run { clock.date.addTimeInterval(0.5); clock.uptime += 0.5 }
                        throw CumulativeWaitFault.injected
                    }
                    try await waits.wait()
                }, loopFallbackSleep: { try await waits.fallback($0) })
            model.setMonitoringMode(.observe)
            let initialTime = clock.date
            attach(model)
            try await wait("fallback after non-cancellation wait error") { await waits.snapshot().1.count == 1 }
            let firstDeadline = model.nextCheckAt
            let firstFallback = await waits.snapshot().1
            check(firstDeadline == initialTime.addingTimeInterval(2) && firstFallback == [1.5],
                  "An unexpected wait failure uses only remaining monotonic time and preserves the original display deadline")
            attach(model, tunnel: false); attach(model); attach(model)
            check(model.nextCheckAt == firstDeadline,
                  "Repeated same-physical TUN notifications cannot postpone the original monitoring deadline")
            if cancelFallback {
                model.setMonitoringMode(.manual)
                await waits.release()
                try await wait("cancelled fallback unwinds") { await waits.snapshot().2 == 0 }
                for _ in 0..<10 { await Task.yield() }
                check(await base.calls == 0 && model.nextCheckAt == nil && !model.busy,
                      "Cancellation during fallback cannot restart monitoring or probe after switching to manual")
            } else {
                clock.date.addTimeInterval(1.5); clock.uptime += 1.5
                await waits.release()
                try await wait("monitoring resumes at original deadline") {
                    let probeCount = await base.calls, schedule = await waits.snapshot()
                    return probeCount == 1 && schedule.0 == 2 && schedule.2 == 1 && !model.busy
                }
                let snapshot = await service.snapshot()
                check(model.isOnline && snapshot.1 == 0,
                      "After the remaining deadline a recovered scheduler performs one check and no authentication")
                model.setMonitoringMode(.manual)
                try await wait("subsequent scheduled wait cancels") { await waits.snapshot().2 == 0 }
            }
        }
        do {
            let (model, _, base, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            await base.enqueue([offline], pause: true); await system.enqueue([offline], pause: true)
            let old = Task { await model.runAutomaticCycle() }
            try await wait("old physical/system probes") {
                let physicalWaiting = await base.waiting, systemWaiting = await system.waiting
                return physicalWaiting && systemWaiting
            }
            model.cancelCurrentOperation()
            await base.release(); await system.release(); await old.value
            try await round(model, base, system)
            check(!model.campusDirectHelpSuggested, "Cancelled late physical/system pair cannot combine with a new failed round")
        }
        do {
            let (model, _, base, system, _, root) = try fixture()
            defer { try? FileManager.default.removeItem(at: root) }
            try await round(model, base, system)
            let json = try JSONSerialization.jsonObject(with: model.diagnosticExportData) as! [String: Any]
            let text = String(data: model.diagnosticExportData, encoding: .utf8)!
            check((json["campusVerification"] as? [String: Any])?["issue"] as? String == "none",
                  "Anonymous JSON includes only the fixed campus failure enum")
            check(!text.contains("en42") && !text.contains("192.0.2") && !text.contains("cumulative-fixture")
                  && !model.diagnosticSummary.contains("en42"),
                  "Diagnostic text and exported samples omit interface identifiers, addresses and route labels")
        }
        var representativeExport: Data?
        for issue: CampusVerificationIssue in [.none, .dnsTimeout, .dns, .dnsTls, .tls, .connection, .http, .page, .binding] {
            let (model, service, base, system, _, root) = try fixture(remembered: true)
            defer { try? FileManager.default.removeItem(at: root) }
            // A genuine fake-model observation supplies a sample whose interface
            // must be anonymized, rather than testing an always-empty export.
            try await round(model, base, system, physical: online, current: online)
            await service.setDiscovery(CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked, issue: issue))
            model.verifyCurrentCampusService()
            try await wait("fixed diagnostic issue \(issue.rawValue)") { !model.busy }
            let data = model.diagnosticExportData
            let json = try JSONSerialization.jsonObject(with: data) as! [String: Any]
            let campus = json["campusVerification"] as? [String: Any]
            let summary = model.diagnosticSummary
            check(campus?["state"] as? String == "unconfirmed" && campus?["issue"] as? String == issue.rawValue,
                  "\(issue.rawValue) model failure exports the same unconfirmed state and fixed issue")
            check(json["summary"] as? String == summary
                  && summary.contains("campus state=unconfirmed issue=\(issue.rawValue)"),
                  "\(issue.rawValue) text and JSON describe the same completed verification round")
            let expectedHint = issue == .none ? "campus.verify.failedHint" : "campus.verify.\(issue.rawValue)Hint"
            check(model.campusVerificationHintKey == expectedHint && SharedCopy.t(expectedHint) != expectedHint,
                  "\(issue.rawValue) diagnosis resolves an actual shared student-facing hint")
            let text = String(data: data, encoding: .utf8)!
            let samples = json["samples"] as? [[String: Any]]
            check(samples?.count == 1 && samples?.first?["interface"] as? String == "有线"
                  && !text.contains("en42") && !text.contains("192.0.2") && !text.contains("cumulative-fixture")
                  && !summary.contains("en42"),
                  "\(issue.rawValue) nonempty export hides interface identifiers, addresses and route labels")
            representativeExport = data
        }
        for issue: CampusVerificationIssue in [.dnsTls, .tls] {
            let (model, service, base, system, _, root) = try fixture(remembered: true)
            defer { try? FileManager.default.removeItem(at: root) }
            await service.setDiscovery(CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked, issue: issue))
            model.setMonitoringMode(.recover)
            try await round(model, base, system, physical: online, current: online)
            let hint = SharedCopy.t("campus.verify.\(issue.rawValue)Hint")
            check(model.automaticCampusVerificationRetryPending && model.campusVerificationFailed
                  && model.memoryVerificationMessage == hint && model.authenticationSummary == hint,
                  "Automatic generic failure preserves the current \(issue.rawValue) explanation without changing retry policy")
            let observed = await service.snapshot()
            check(observed.1 == 0 && !model.currentNetworkAuthorized,
                  "Automatic phase diagnosis grants no trust and submits no credentials")
            model.setMonitoringMode(.manual)
        }
        do {
            let phases = CumulativeTLSPhases(), bound = route()
            let portal = PortalClient(probeTimeout: 0.5, authenticationTimeout: 0.5,
                physicalSelectionProvider: { .bound(bound) }, targetedRouteProvider: { _ in .bound(bound) },
                physicalTransportFactory: { route, cookies in
                    CampusPhysicalTransport(route: route, cookies: cookies, routeIsCurrent: { $0 == bound },
                        resolve: { _, _, _, _ in try phases.associated() }, addressResolver: CampusPhysicalAddressResolver(),
                        encrypted: { _, _, _, _, _ in try phases.encrypted() },
                        transfer: { url, _, _, _, _, _, _, _ in try phases.target(url) })
                })
            let (model, _, _, _, _, root) = try fixture(remembered: true, clientOverride: portal)
            defer { try? FileManager.default.removeItem(at: root) }
            let memoryURL = root.appendingPathComponent("memory.json")
            let memoryBefore = try Data(contentsOf: memoryURL)
            for (phase, issue) in [(CumulativeTLSPhases.Phase.dnsTLS, CampusVerificationIssue.dnsTls), (.campusTLS, .tls)] {
                phases.use(phase)
                model.verifyCurrentCampusService()
                try await wait("real physical-to-model \(issue.rawValue)") { !model.busy }
                let hint = "campus.verify.\(issue.rawValue)Hint", message = SharedCopy.t(hint)
                check(model.lastDiscovery?.issue == issue && model.campusVerificationDisplayState == "unconfirmed"
                      && model.campusVerificationHintKey == hint && model.memoryVerificationMessage == message
                      && model.authenticationSummary == message,
                      "Real transport → Portal → model uses the same \(issue.rawValue) hint in visible and accessible text")
                let data = model.diagnosticExportData
                if issue == .dnsTls { representativeExport = data }
                let json = try JSONSerialization.jsonObject(with: data) as! [String: Any]
                let campus = json["campusVerification"] as! [String: Any]
                let copied = model.diagnosticSummary
                check(campus["issue"] as? String == issue.rawValue && campus["state"] as? String == "unconfirmed"
                      && copied.contains("campus state=unconfirmed issue=\(issue.rawValue)")
                      && json["summary"] as? String == copied && copied.contains(message),
                      "Real \(issue.rawValue) discovery reaches copied summary and JSON with its actual phase")
                let text = String(data: data, encoding: .utf8)!
                let memoryAfter = try Data(contentsOf: memoryURL)
                check(!text.contains("192.0.2") && !text.contains("en42") && !text.contains("cumulative-fixture")
                      && !model.currentNetworkAuthorized && memoryAfter == memoryBefore,
                      "End-to-end TLS diagnostics retain anonymous output, original memory and no session trust")
            }
            check(phases.counts == (2, 1, 1), "Each TLS failure stops after its own failing phase without retries or plaintext requests")
            phases.use(.healthy)
            model.verifyCurrentCampusService()
            try await wait("phase-specific error cleared by fresh success") { !model.busy }
            check(model.lastDiscovery?.issue == CampusVerificationIssue.none && model.campusVerificationDisplayState == "confirmed"
                  && model.memoryVerificationMessage == SharedCopy.t("campus.verify.confirmed")
                  && !model.diagnosticSummary.contains("issue=dnsTls") && !model.diagnosticSummary.contains("issue=tls"),
                  "Fresh success replaces both old TLS phases in UI and copied diagnosis")
        }
        if let destination = ProcessInfo.processInfo.environment["SZU_TEST_DIAGNOSTIC_EXPORT"], !destination.isEmpty,
           let representativeExport {
            try representativeExport.write(to: URL(fileURLWithPath: destination), options: .atomic)
        }
        print("Cumulative recovery: \(assertions) assertions, \(failures.count) failures; real model with fake services only.")
        if !failures.isEmpty { exit(1) }
    }
}
