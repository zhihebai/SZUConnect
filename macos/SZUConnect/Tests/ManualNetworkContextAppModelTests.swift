import Foundation
import Darwin

/// Non-cooperative captures deliberately return after cancellation. The model,
/// rather than cancellation of this fake, must reject their obsolete ownership.
private actor ManualContextReader {
    private(set) var calls = 0
    private var pending: [Int: CheckedContinuation<PathObservation, Error>] = [:]
    private var observers: [Int: [CheckedContinuation<Void, Never>]] = [:]
    func read() async throws -> PathObservation {
        calls += 1
        let id = calls
        return try await withCheckedThrowingContinuation { continuation in
            pending[id] = continuation
            observers.removeValue(forKey: id)?.forEach { $0.resume() }
        }
    }
    func waitForCall(_ id: Int) async {
        if pending[id] != nil { return }
        await withCheckedContinuation { observers[id, default: []].append($0) }
    }
    func complete(_ id: Int, with observation: PathObservation) {
        guard let continuation = pending.removeValue(forKey: id) else { fatalError("Missing fake manual read") }
        continuation.resume(returning: observation)
    }
    func fail(_ id: Int) {
        guard let continuation = pending.removeValue(forKey: id) else { fatalError("Missing fake manual read") }
        continuation.resume(throwing: CocoaError(.fileReadUnknown))
    }
}

private actor ManualContextService: ConnectionService {
    enum Phase: String, Hashable, Sendable { case preflight, submission, confirmation }
    struct Snapshot {
        let probes: [CampusPhysicalRoute]
        let discoveries: [CampusPhysicalRoute]
        let submissions: [CampusPhysicalRoute]
    }
    private var route: CampusPhysicalRoute?
    private var online = ["en0": true, "en12": true]
    private var probes: [CampusPhysicalRoute] = []
    private var discoveries: [CampusPhysicalRoute] = []
    private var submissions: [CampusPhysicalRoute] = []
    private var acceptedInterfaces: Set<String> = []
    private var gatedPhase: Phase?
    private var pendingPhases: [Phase: CheckedContinuation<Void, Never>] = [:]
    private var phaseObservers: [Phase: [CheckedContinuation<Void, Never>]] = [:]
    func setOnline(_ value: Bool, interface: String = "en0") { online[interface] = value }
    func pause(_ phase: Phase) { gatedPhase = phase }
    func waitForPhase(_ phase: Phase) async {
        if pendingPhases[phase] != nil { return }
        await withCheckedContinuation { phaseObservers[phase, default: []].append($0) }
    }
    func release(_ phase: Phase) {
        guard let continuation = pendingPhases.removeValue(forKey: phase) else { fatalError("Missing fake credential phase") }
        continuation.resume()
    }
    private func suspendIfRequested(_ phase: Phase) async {
        guard gatedPhase == phase else { return }
        gatedPhase = nil
        await withCheckedContinuation { continuation in
            pendingPhases[phase] = continuation
            phaseObservers.removeValue(forKey: phase)?.forEach { $0.resume() }
        }
    }
    func setExpectedPhysicalRoute(_ value: CampusPhysicalRoute?) -> Bool { route = value; return value != nil }
    func resetConnections() { route = nil }
    private func bound() -> CampusPhysicalRoute {
        guard let route, ["en0", "en12"].contains(route.interfaceName) else { fatalError("Unbound fake manual request") }
        return route
    }
    func probe() async -> ConnectivityReport {
        let captured = bound(); probes.append(captured)
        let result = online[captured.interfaceName] == true
        if acceptedInterfaces.contains(captured.interfaceName) { await suspendIfRequested(.confirmation) }
        return ConnectivityReport(apple: result, microsoft: result)
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(bound())
        return CampusDiscovery(teaching: .confirmed, dormitory: .notChecked, teachingClientBound: true)
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        await attemptLogin(credentials: credentials, area: area, requireBoundClientConfiguration: false)
    }
    func attemptLogin(credentials: Credentials, area: CampusArea, requireBoundClientConfiguration: Bool) async -> LoginAttempt {
        precondition(credentials.account == "manual-context-fixture-account"
            && credentials.password == "manual-context-fixture-password" && area == .teaching,
            "Only fake single-area credentials may be submitted")
        let captured = bound()
        await suspendIfRequested(.preflight)
        guard !Task.isCancelled, captured == route else { return LoginAttempt(outcome: .cancelled) }
        submissions.append(captured)
        await suspendIfRequested(.submission)
        online[captured.interfaceName] = true
        acceptedInterfaces.insert(captured.interfaceName)
        return .success("Manual context fixture accepted")
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { fatalError("Use staged fake discovery") }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("Use classified fake attempts") }
    func snapshot() -> Snapshot { Snapshot(probes: probes, discoveries: discoveries, submissions: submissions) }
}

private actor ManualContextWaiter {
    private var calls: [Int] = []
    private var pending: [Int: CheckedContinuation<Void, Error>] = [:]
    private var observers: [Int: [CheckedContinuation<Void, Never>]] = [:]
    func sleep(_ seconds: Int) async throws {
        calls.append(seconds)
        let id = calls.count
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                pending[id] = continuation
                observers.removeValue(forKey: id)?.forEach { $0.resume() }
            }
        } onCancel: { Task { await self.cancel(id) } }
    }
    private func cancel(_ id: Int) { pending.removeValue(forKey: id)?.resume(throwing: CancellationError()) }
    func waitForCall(_ id: Int) async {
        if calls.count >= id { return }
        await withCheckedContinuation { observers[id, default: []].append($0) }
    }
    func release(_ id: Int) {
        guard let continuation = pending.removeValue(forKey: id) else { fatalError("Missing fake monitoring wait") }
        continuation.resume()
    }
    func delays() -> [Int] { calls }
}

@MainActor private final class ManualContextClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    var monotonic: TimeInterval = 1_000
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds); monotonic += seconds }
}

@main @MainActor
struct ManualNetworkContextAppModelTests {
    private static var assertions = 0
    private static func check(_ value: Bool, _ message: String) {
        precondition(value, "FAIL: " + message); assertions += 1; print("PASS: " + message)
    }
    private static func folder() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("szu-manual-context-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    private static func route(_ wired: Bool, primary: Bool = true) -> CampusPhysicalRoute {
        let descriptor = NetworkDescriptor(kind: wired ? "ethernet" : "wifi", raw: [
            "gw": wired ? "a4:5e:60:93:19:02" : "a4:5e:60:93:19:01",
            "gwip": wired ? "198.51.100.1" : "192.0.2.1",
            "net": wired ? "198.51.100.0/24" : "192.0.2.0/24",
            "dhcp": wired ? "198.51.100.2" : "192.0.2.2",
            "dns": wired ? "198.51.100.53" : "192.0.2.53"])
        return CampusPhysicalRoute(interfaceName: wired ? "en12" : "en0", interfaceIndex: wired ? 12 : 1,
            addresses: [wired ? "198.51.100.12" : "192.0.2.11"],
            dnsServers: [wired ? "198.51.100.53" : "192.0.2.53"],
            identity: wired ? "manual-fixture-ethernet" : "manual-fixture-wifi",
            serviceID: wired ? "manual-fixture-wired-service" : "manual-fixture-wifi-service",
            descriptor: descriptor, isSystemPrimary: primary)
    }
    private static func path(_ wired: Bool) -> PathObservation {
        let candidates = wired ? [route(false, primary: false), route(true)] : [route(false)]
        var continuity = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: wired ? "manual-topology-wired" : "manual-topology-wifi"), linkActive: true)
        continuity.requiresPhysicalBinding = true; continuity.hasUsablePhysicalLink = true; continuity.tunnel = true
        continuity.systemPhysicalLabel = wired ? "有线 en12" : "Wi-Fi en0"
        continuity.connectedPhysicalLabels = candidates.map { $0.interfaceName }
        return PathObservation(usable: true, interfaces: "manual fixture topology", constrained: false,
            continuity: continuity, physicalCandidates: candidates)
    }
    private static func fixture(_ root: URL, reader: ManualContextReader, service: ManualContextService,
                                clock providedClock: ManualContextClock? = nil, waiter: ManualContextWaiter? = nil) -> AppModel {
        let clock = providedClock ?? ManualContextClock()
        let sleep: (@Sendable (Int) async throws -> Void)?
        if let waiter { sleep = { seconds in try await waiter.sleep(seconds) } }
        else { sleep = nil }
        let model = AppModel(client: service, isolated: true, now: { clock.date }, monotonicNow: { clock.monotonic },
            recoveryStore: root.appendingPathComponent("recovery.json"),
            isolatedCredentials: Credentials(account: "manual-context-fixture-account", password: "manual-context-fixture-password"),
            memoryStore: root.appendingPathComponent("network-memory.json"),
            runAutomaticLoopInIsolation: waiter != nil,
            loopSleep: sleep,
            recordStoreDirectory: root,
            networkInformationReader: { await path(false) },
            manualNetworkReader: { try await reader.read() }, credentialDeletion: {})
        model.setProbeMode(.complete); model.setMonitoringMode(.recover)
        model.applyEnvironmentObservation(path(false))
        return model
    }
    /// Capture real tracked task handles only for deterministic waiting. No production
    /// test API or guessed scheduling delay is needed to drain a cancelled reader.
    private static func tasks(_ model: AppModel) -> [Task<Void, Never>] {
        guard let value = Mirror(reflecting: model).children.first(where: { $0.label == "backgroundTasks" })?.value
            as? [UUID: Task<Void, Never>] else { fatalError("Tracked task fixture contract changed") }
        return Array(value.values)
    }
    private static func finish(_ captured: [Task<Void, Never>]) async { for task in captured { await task.value } }
    private static func finishCurrent(_ model: AppModel) async { await finish(tasks(model)) }
    private static func completeRead(_ model: AppModel, reader: ManualContextReader, id: Int, result: PathObservation) async {
        await reader.waitForCall(id)
        let capture = tasks(model)
        await reader.complete(id, with: result)
        await finish(capture)
        await finishCurrent(model)
    }
    private static func primeOnline(_ model: AppModel, reader: ManualContextReader) async -> AppModel.ForcedLoginConfirmation {
        model.connect()
        await completeRead(model, reader: reader, id: 1, result: path(false))
        guard let confirmation = model.forcedLoginConfirmation() else { fatalError("No fixture forced-login confirmation") }
        return confirmation
    }

    static func main() async throws {
        setbuf(stdout, nil)
        alarm(60) // Deadlock guard only; no timing assertion or scheduler sleep.
        defer { alarm(0) }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            _ = await primeOnline(model, reader: reader)
            model.checkConnection(); await reader.waitForCall(2)
            let captured = tasks(model)
            var heartbeat = false
            await Task { @MainActor in heartbeat = true }.value
            model.checkConnection(); model.connect(); await model.runAutomaticCycle()
            let before = await service.snapshot()
            let reads = await reader.calls
            check(heartbeat && model.busy && reads == 2 && before.probes == [route(false)] && before.submissions.isEmpty,
                  "A suspended worker capture keeps the main actor responsive, coalesces repeated clicks and blocks automatic admission")
            await reader.complete(2, with: path(true)); await finish(captured); await finishCurrent(model)
            let after = await service.snapshot()
            check(model.usingWiredCampusRoute && after.probes == [route(false), route(true)] && after.submissions.isEmpty
                  && !model.networkRemembered && !model.currentNetworkAuthorized,
                  "A fresh unknown Ethernet observation is fully applied before a manual detection probes its own source and DNS")
        }

        for interruption in ["cancel", "samePathNotification", "changedPath", "refresh", "identificationRefresh", "sleep", "close", "mode", "area", "accountDeletion"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            model.connect(); await reader.waitForCall(1)
            let captured = tasks(model)
            switch interruption {
            case "cancel": model.cancelCurrentOperation()
            case "samePathNotification": model.applyEnvironmentObservation(path(false))
            case "changedPath": model.applyEnvironmentObservation(path(true))
            case "refresh": model.refreshDisplayedNetwork()
            case "identificationRefresh": model.refreshPhysicalIdentification()
            case "sleep": model.setSleeping(true)
            case "close": model.cancelPendingManualNetworkRead()
            case "mode": model.setMonitoringMode(.manual)
            case "area": model.area = .dormitory
            case "accountDeletion": await model.deleteSavedCredentials()
            default: fatalError("Unknown fixture interruption")
            }
            check(!model.busy, "\(interruption): pending capture ownership is revoked immediately")
            await reader.complete(1, with: path(false)); await finish(captured); await finishCurrent(model)
            let snapshot = await service.snapshot()
            check(snapshot.probes.isEmpty && snapshot.discoveries.isEmpty && snapshot.submissions.isEmpty && !model.busy,
                  "\(interruption): deliberately late capture cannot start detection, discovery or credentials")
            if interruption == "changedPath" {
                check(model.usingWiredCampusRoute, "A late Wi-Fi capture cannot replace the newer physical Ethernet context")
            }
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            _ = await primeOnline(model, reader: reader)
            let original = await service.snapshot()
            model.errorMessage = "Fixture unrelated storage error"
            model.connect(); await reader.waitForCall(2)
            let captured = tasks(model)
            await reader.fail(2); await finish(captured)
            let failure = await service.snapshot()
            check(model.manualNetworkReadError == SharedCopy.t("env.refreshFailed") && !model.busy
                  && failure.probes == original.probes && failure.submissions.isEmpty,
                  "A failed fresh capture is visible and never falls back to the previously online Wi-Fi")
            check(model.errorMessage == "Fixture unrelated storage error", "Capture failure preserves an unrelated storage error")
            model.checkConnection()
            await completeRead(model, reader: reader, id: 3, result: path(false))
            check(model.manualNetworkReadError == nil && model.errorMessage == "Fixture unrelated storage error"
                  && model.lastInternetCheck?.base.allPassed == true,
                  "Successful capture recovery clears only its own error and resumes the ordinary detection flow")
        }

        for change in ["result", "probeMode", "probeModeRoundTrip", "websiteStrategy", "area", "mode", "modeRoundTrip", "route", "close"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            let confirmation = await primeOnline(model, reader: reader)
            check(model.canContinueForcedLogin(confirmation), "\(change): confirmation is captured while its displayed result is current")
            switch change {
            case "result":
                model.checkConnection()
                await completeRead(model, reader: reader, id: 2, result: path(false))
            case "probeMode": model.setProbeMode(.quick)
            case "probeModeRoundTrip": model.setProbeMode(.quick); model.setProbeMode(.complete)
            case "websiteStrategy": model.setWebsiteStrategy(.direct)
            case "area": model.area = .dormitory
            case "mode": model.setMonitoringMode(.observe)
            case "modeRoundTrip": model.setMonitoringMode(.manual); model.setMonitoringMode(.recover)
            case "route": model.applyEnvironmentObservation(path(true))
            case "close": model.cancelPendingManualNetworkRead()
            default: fatalError("Unknown confirmation change")
            }
            let before = await reader.calls
            check(!model.canContinueForcedLogin(confirmation), "\(change): a displayed confirmation expires when its bound context changes")
            model.connectEvenIfOnline(confirmation: confirmation)
            model.connectEvenIfOnline(confirmation: nil)
            let snapshot = await service.snapshot()
            check(await reader.calls == before && snapshot.submissions.isEmpty,
                  "\(change): stale or default-cancel confirmation cannot acquire a new snapshot or submit credentials")
        }

        for duringCapture in ["newObservation", "routeNotification", "close"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            let confirmation = await primeOnline(model, reader: reader)
            let before = await service.snapshot()
            model.connectEvenIfOnline(confirmation: confirmation); await reader.waitForCall(2)
            let captured = tasks(model)
            if duringCapture == "routeNotification" { model.applyEnvironmentObservation(path(true)) }
            if duringCapture == "close" { model.cancelPendingManualNetworkRead() }
            await reader.complete(2, with: duringCapture == "newObservation" ? path(true) : path(false))
            await finish(captured); await finishCurrent(model)
            let after = await service.snapshot()
            check(after.probes.count == before.probes.count && after.submissions.isEmpty && !model.busy,
                  "\(duringCapture): a confirmation cannot be transferred across capture application or cancellation into an admitted force login")
            if duringCapture != "close" { check(model.usingWiredCampusRoute, "\(duringCapture): the current Ethernet context survives stale confirmation") }
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            let confirmation = await primeOnline(model, reader: reader)
            model.connectEvenIfOnline(confirmation: confirmation)
            await completeRead(model, reader: reader, id: 2, result: path(false))
            let snapshot = await service.snapshot()
            check(snapshot.submissions == [route(false)] && snapshot.discoveries == [route(false)] && model.isOnline,
                  "An unchanged explicit confirmation admits exactly one fake teaching submission and same-binding confirmation")
            model.connectEvenIfOnline(confirmation: confirmation)
            let reads = await reader.calls, replay = await service.snapshot()
            check(reads == 2 && replay.submissions.count == 1,
                  "A consumed confirmation cannot be replayed for another capture or submission")
        }

        for phase in [ManualContextService.Phase.preflight, .submission, .confirmation] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService()
            let model = fixture(root, reader: reader, service: service)
            let confirmation = await primeOnline(model, reader: reader)
            await service.setOnline(false); await service.pause(phase)
            model.connect(); await reader.waitForCall(2)
            let capture = tasks(model)
            await reader.complete(2, with: path(false)); await finish(capture)
            await service.waitForPhase(phase)
            let operation = tasks(model)
            model.checkConnection(); model.connect(); model.connectEvenIfOnline(confirmation: confirmation)
            let reads = await reader.calls, competing = await service.snapshot()
            check(model.busy && reads == 2 && competing.submissions.count == (phase == .preflight ? 0 : 1),
                  "\(phase): competing detection, login and force clicks cannot begin a new capture or preempt the credential owner")
            check(competing.probes.allSatisfy { $0 == route(false) }
                  && competing.discoveries.allSatisfy { $0 == route(false) }
                  && competing.submissions.allSatisfy { $0 == route(false) },
                  "\(phase): all observed credential-operation stages retain their original physical source and DNS")
            await service.release(phase); await finish(operation)
            let completed = await service.snapshot()
            check(model.isOnline && completed.submissions == [route(false)],
                  "\(phase): the original credential operation finishes with exactly one submission and a same-binding confirmation")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let reader = ManualContextReader(), service = ManualContextService(), waiter = ManualContextWaiter()
            let clock = ManualContextClock()
            let model = fixture(root, reader: reader, service: service, clock: clock, waiter: waiter)
            for id in 1...3 {
                await waiter.waitForCall(id)
                let delay = (await waiter.delays())[id - 1]
                clock.advance(TimeInterval(delay)); await waiter.release(id)
            }
            await waiter.waitForCall(4)
            let original = model.nextCheckAt
            check((await waiter.delays()).last == 60 && original == clock.date.addingTimeInterval(60),
                  "A virtual healthy monitoring sequence establishes its ordinary sixty-second deadline")
            model.connect(); await reader.waitForCall(1)
            let capture = tasks(model)
            model.cancelCurrentOperation()
            await waiter.waitForCall(5)
            check((await waiter.delays()).last == 60 && model.nextCheckAt == original,
                  "Cancelling a pending manual capture preserves an existing sixty-second plan rather than advancing it to thirty seconds")
            await reader.complete(1, with: path(false)); await finish(capture)
            check((await service.snapshot()).submissions.isEmpty, "A late cancelled capture cannot authenticate while ordinary monitoring is resumed")
            check(await model.prepareForAppMaintenance(), "The fake monitoring waiter remains cancellable and drains without real time sleeps")
        }
        print("Manual network context AppModel tests: \(assertions) assertions passed; gated fake captures, services and virtual monitoring only")
    }
}
