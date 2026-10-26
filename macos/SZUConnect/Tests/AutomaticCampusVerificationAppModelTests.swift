import Foundation
import Darwin

/// Tests known-attachment confirmation with an in-memory service and a virtual
/// clock. No PortalClient, application, real network, Keychain or preferences.
private actor AutomaticCampusService: ConnectionService {
    struct Snapshot: Sendable {
        let probes: Int
        let discoveries: [CampusArea]
        let completed: Int
        let submissions: Int
        let routes: [String?]
    }
    private var route: CampusPhysicalRoute?
    private var report = ConnectivityReport(apple: true, microsoft: true)
    private var reports: [ConnectivityReport] = []
    private var replies: [CampusDiscovery] = []
    private var probes = 0
    private var discoveries: [CampusArea] = []
    private var routes: [String?] = []
    private var completed = 0
    private var submissions = 0
    private var gateNext = false
    private var gate: CheckedContinuation<Void, Never>?

    func configure(replies: [CampusDiscovery] = [], reports: [ConnectivityReport] = [], gated: Bool = false) {
        self.replies = replies; self.reports = reports; gateNext = gated
    }
    func setExpectedPhysicalRoute(_ value: CampusPhysicalRoute?) -> Bool { route = value; return true }
    func resetConnections() { route = nil }
    func probe() async -> ConnectivityReport {
        probes += 1
        if !reports.isEmpty { report = reports.removeFirst() }
        return report
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(area); routes.append(route?.identity)
        let reply = replies.isEmpty ? CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
            : replies.removeFirst()
        if gateNext {
            gateNext = false
            // Deliberately ignores cancellation; AppModel must reject stale results.
            await withCheckedContinuation { gate = $0 }
        }
        completed += 1
        return reply
    }
    func isWaiting() -> Bool { gate != nil }
    func release() {
        guard let continuation = gate else { fatalError("No gated fake discovery") }
        gate = nil; continuation.resume()
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        guard credentials.account == "automatic-campus-fixture-account",
              credentials.password == "automatic-campus-fixture-password", area == .teaching else {
            fatalError("Unexpected simulated credentials or area")
        }
        submissions += 1
        return .success("Fixture accepted")
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { fatalError("Use staged discovery") }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("Use classified attempt") }
    func snapshot() -> Snapshot {
        Snapshot(probes: probes, discoveries: discoveries, completed: completed, submissions: submissions, routes: routes)
    }
}

@MainActor private final class AutomaticCampusClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    var uptime: TimeInterval = 100
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds); uptime += seconds }
}

@main @MainActor
struct AutomaticCampusVerificationAppModelTests {
    private static var assertions = 0
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let recordID = "01ab23cd"
    private static let both = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let failed = CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
    private static let online = ConnectivityReport(apple: true, microsoft: true)
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private nonisolated static let descriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "02:00:00:00:ab:01", "gwip": "192.0.2.1", "net": "192.0.2.0/24",
        "dhcp": "192.0.2.2", "dns": "192.0.2.3"
    ])
    private static func check(_ value: Bool, _ text: String) {
        guard value else { fatalError("FAIL: \(text)") }
        assertions += 1; print("PASS: \(text)")
    }
    private static func folder() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-auto-campus-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }
    private static func seed(_ root: URL, area: String = "teaching") throws -> Data {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        guard memory.remember(descriptor, area: area, proofTeaching: true, proofDormitory: area == "dormitory",
            now: date.addingTimeInterval(-3_600), newID: recordID) != nil else { fatalError("Invalid memory fixture") }
        let bytes = memory.encoded()
        try NetworkMemoryFile.write(bytes, to: root.appendingPathComponent("network-memory.json"))
        return bytes
    }
    private static func route(_ lease: String = "A") -> CampusPhysicalRoute {
        CampusPhysicalRoute(interfaceName: "en12", interfaceIndex: 12, addresses: ["192.0.2.10"],
            dnsServers: ["192.0.2.3"], identity: "fixture-confirmation-route-\(lease)", serviceID: "fixture-confirmation-service",
            descriptor: descriptor, isSystemPrimary: true)
    }
    private static func attach(_ model: AppModel, lease: String = "A", usable: Bool = true) {
        model.receivePath(usable: usable, interfaces: "有线 en12", identity: NetworkIdentity(fingerprint: "fixture-confirmation-network"),
            linkActive: usable, now: date, descriptors: [descriptor], physicalConnected: usable,
            systemPhysicalLabel: "有线 en12", connectedPhysicalLabels: usable ? ["有线 en12"] : [],
            authenticationRoute: usable ? route(lease) : nil, requiresPhysicalBinding: true)
    }
    private static func fixture(_ root: URL, _ service: AutomaticCampusService, _ clock: AutomaticCampusClock,
                                mode: MonitoringMode = .recover, readOnly: Bool = false,
                                area: CampusArea = .teaching) -> AppModel {
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: { clock.date }, monotonicNow: { clock.uptime },
            recoveryStore: root.appendingPathComponent("recovery.json"),
            isolatedCredentials: Credentials(account: "automatic-campus-fixture-account", password: "automatic-campus-fixture-password"),
            memoryStore: root.appendingPathComponent("network-memory.json"), verificationSleep: { _ in })
        model.area = area; model.setProbeMode(.complete); model.setMonitoringMode(mode)
        attach(model)
        return model
    }
    private static func waitUntil(_ text: String, _ predicate: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await predicate() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(text)")
    }
    private static func finishManual(_ model: AppModel) async throws {
        try await waitUntil("manual completion") { !model.busy }
    }
    static func main() async throws {
        setbuf(stdout, nil)

        // Selecting automatic must retain either known entry when public probes
        // have no portal hint and both independently checked services respond.
        for rememberedArea in [CampusArea.teaching, .dormitory] {
            for mode in [ConnectivityProbeMode.quick, .complete] {
                let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
                let bytes = try seed(root, area: rememberedArea.rawValue)
                let service = AutomaticCampusService(), clock = AutomaticCampusClock()
                let model = fixture(root, service, clock, area: .automatic)
                model.setProbeMode(mode)
                let report = mode == .quick
                    ? ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                        microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick)
                    : online
                await service.configure(replies: [both], reports: [report])
                await model.runAutomaticCycle()
                check(model.currentCampusRouting == .selected(rememberedArea, allowUnsentFallback: false),
                      "Automatic selection retains the verified remembered \(rememberedArea.rawValue) entry")
                check(model.requiredCampusVerificationState == "confirmed" && model.autoLoginKey == "ready"
                      && !model.entrySelectionNeeded && model.readiness.tone == "success",
                      "Known \(rememberedArea.rawValue) \(mode) verification and recovery readiness agree")
                await service.configure(replies: [both], reports: [report])
                model.verifyCurrentCampusService(); try await finishManual(model)
                check(model.currentCampusRouting == .selected(rememberedArea, allowUnsentFallback: false)
                      && model.autoLoginKey == "ready" && !model.entrySelectionNeeded,
                      "Manual service-only verification uses the same remembered automatic entry")
                check(await service.snapshot().submissions == 0 && !model.currentNetworkAuthorized,
                      "Remembered candidate selection grants no trust and sends no credentials")
                check(try Data(contentsOf: root.appendingPathComponent("network-memory.json")) == bytes,
                      "Read-only selection and verification preserve network memory bytes")
            }
        }

        // A successful public probe is followed by independent, credential-free
        // confirmation for a remembered attachment, without changing its saved bytes.
        for area in [CampusArea.teaching, .dormitory] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let bytes = try seed(root, area: area.rawValue)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock, area: area)
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 1,
                  "Online remembered \(area.rawValue) attachment automatically confirms its campus service")
            check(model.isOnline && model.requiredCampusVerificationState == "confirmed"
                  && model.readiness.title == "ready.title.ready" && model.readiness.tone == "success",
                  "Independent confirmed proof produces the completed green readiness state")
            let confirmedSnapshot = await service.snapshot()
            check(!model.currentNetworkAuthorized && confirmedSnapshot.submissions == 0,
                  "Automatic confirmation neither grants session trust nor submits credentials")
            check(try Data(contentsOf: root.appendingPathComponent("network-memory.json")) == bytes,
                  "Automatic confirmation preserves exact saved network bytes")
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 1,
                  "Repeated passing checks do not repeatedly confirm the same attachment")
        }

        // The manual check button uses the same independent check-only confirmation.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            model.checkConnection(); try await finishManual(model)
            check(await service.snapshot().discoveries.count == 1 && model.requiredCampusVerificationState == "confirmed",
                  "Check connection also completes remembered service confirmation in recovery mode")
            check(await service.snapshot().submissions == 0, "Check-only confirmation sends no credentials")
        }

        // A real path transition invalidates displayed proof, including the same
        // stable network identity coming back after cable removal.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            await model.runAutomaticCycle()
            attach(model, usable: false); attach(model, lease: "B")
            check(model.networkRemembered && model.requiredCampusVerificationState == "notChecked",
                  "Reattached remembered wire keeps eligibility but clears the old service proof")
            await service.configure(replies: [both], gated: true)
            let task = Task { await model.runAutomaticCycle() }
            try await waitUntil("automatic service confirmation") { await service.isWaiting() }
            check(model.isOnline && model.campusVerificationInProgress && model.readiness.title == "ready.title.checking",
                  "In-flight independent confirmation visibly describes its checking stage")
            await service.release(); await task.value
            check(model.requiredCampusVerificationState == "confirmed" && model.readiness.tone == "success",
                  "Reattached wire becomes confirmed without clicking a separate verification button")
            check(await service.snapshot().routes == [route().identity, route("B").identity],
                  "Each independent confirmation uses the currently selected physical route")
        }

        // Backoff is measured against an injected clock; no real timing assertion.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            await service.configure(replies: [failed, both])
            await model.runAutomaticCycle()
            check(model.isOnline && model.requiredCampusVerificationState == "unconfirmed"
                  && model.readiness.tone != "success" && model.memoryVerificationMessage == SharedCopy.t("campus.verify.autoFailed"),
                  "Service failure stays explicit while public connectivity remains online")
            await model.runAutomaticCycle(); clock.advance(29); await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 1,
                  "Repeated online checks before the retry deadline do not flood campus service")
            clock.advance(1); await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 2 && model.requiredCampusVerificationState == "confirmed",
                  "The next check at the virtual retry deadline can confirm a recovered campus service")
        }

        // A normally skipped quick endpoint still permits independent campus
        // confirmation; an observed timeout is a different connectivity fact.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            model.setProbeMode(.quick)
            let quick = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick)
            await service.configure(reports: [quick])
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 1 && model.requiredCampusVerificationState == "confirmed",
                  "A quick success with the other endpoint genuinely skipped automatically confirms campus service")
            check(model.lastInternetCheck?.base.mode == .quick
                  && model.lastInternetCheck?.base.microsoftResult.state == .notCompleted,
                  "Quick confirmation preserves the actual skipped endpoint rather than fabricating a full pass")
            check(await service.snapshot().submissions == 0, "Quick confirmation remains credential-free")
        }
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            model.setProbeMode(.quick)
            let partial = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .timeout, elapsed: 1), mode: .quick)
            await service.configure(reports: [partial])
            await model.runAutomaticCycle()
            let quickSnapshot = await service.snapshot()
            check(model.isOnline && quickSnapshot.discoveries.isEmpty && quickSnapshot.submissions == 0
                  && model.requiredCampusVerificationState != "confirmed",
                  "An observed quick timeout does not enter normal online campus confirmation or authenticate early")
            check(model.lastInternetCheck?.base.microsoftResult.state == .timeout,
                  "An observed timeout remains a timeout rather than being treated as a skipped endpoint")
            model.setProbeMode(.complete); clock.advance(46)
            await service.configure(replies: [both], reports: [partial, partial, online])
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.isEmpty && model.lastInternetCheck?.base.mode == .complete,
                  "The subsequent complete partial round retains the existing confirmation threshold")
            await model.runAutomaticCycle()
            let recoverySnapshot = await service.snapshot()
            check(recoverySnapshot.discoveries.count == 1 && recoverySnapshot.submissions == 1,
                  "Persistent complete partial evidence still reaches the existing single bounded recovery attempt")
        }
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            clock.advance(46) // Expire only the virtual path-change quick-probe burst.
            let partial = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .timeout, elapsed: 1), mode: .complete)
            await service.configure(reports: [partial])
            await model.runAutomaticCycle()
            let partialSnapshot = await service.snapshot()
            check(model.lastInternetCheck?.base.mode == .complete
                  && model.lastInternetCheck?.hasCampusRecoveryEvidence == true,
                  "A genuine complete partial report remains recovery evidence")
            check(partialSnapshot.discoveries.isEmpty && partialSnapshot.submissions == 0
                  && model.requiredCampusVerificationState != "confirmed",
                  "The first complete partial report does not prematurely discover or authenticate")
        }

        // Partial/redirect evidence stays in the original recovery flow. An
        // explicit read-only verification must still reject inapplicable proof.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock, area: .automatic)
            let report = ConnectivityReport(
                appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .redirect, elapsed: 0.01, campusAreaHint: .dormitory), mode: .complete)
            let reply = CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)
            await service.configure(replies: [reply], reports: [report])
            await model.runAutomaticCycle()
            let partialSnapshot = await service.snapshot()
            check(model.isOnline && partialSnapshot.discoveries.isEmpty && partialSnapshot.submissions == 0,
                  "A partial online redirect does not enter the normal online confirmation helper")
            model.verifyCurrentCampusService(); try await finishManual(model)
            check(model.requiredCampusVerificationState == "unconfirmed"
                  && model.campusVerificationDisplayState != "confirmed" && model.readiness.tone != "success"
                  && model.memoryVerificationMessage == SharedCopy.t("campus.verify.failed"),
                  "Explicit verification cannot turn an inapplicable current candidate green")
            await service.configure(replies: [both], reports: [online])
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 2 && model.requiredCampusVerificationState == "confirmed",
                  "A later healthy online report obtains fresh proof rather than borrowing a rejected candidate")
            check(await service.snapshot().submissions == 0 && !model.currentNetworkAuthorized,
                  "Candidate conflict handling never authenticates or grants session trust")
        }

        // A successful endpoint cannot retain a redirect hint. With two truly
        // conflicting hints both endpoints redirect, so public online is false.
        let successHint = ConnectivityEndpointResult(state: .success, elapsed: 0.01, campusAreaHint: .teaching)
        check(successHint.campusAreaHint == nil,
              "The endpoint constructor prevents a synthetic online success from carrying a redirect hint")
        let conflictingReport = ConnectivityReport(
            appleResult: .init(state: .redirect, elapsed: 0.01, campusAreaHint: .teaching),
            microsoftResult: .init(state: .redirect, elapsed: 0.01, campusAreaHint: .dormitory), mode: .complete)
        check(!conflictingReport.online,
              "Two genuine conflicting endpoint hints cannot enter the online-only confirmation path")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: conflictingReport,
              discovery: both, physicallyBound: true, rememberedArea: .teaching) == .conflictingHints,
              "Even two confirmed services cannot override a current physical candidate conflict")
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock, area: .automatic)
            await service.configure(reports: [conflictingReport])
            model.checkConnection(); try await finishManual(model)
            let conflictCheckSnapshot = await service.snapshot()
            check(!model.isOnline && conflictCheckSnapshot.discoveries.isEmpty,
                  "A check-only round with conflicting redirects remains offline without guessing a campus candidate")
            await service.configure(replies: [both])
            model.verifyCurrentCampusService(); try await finishManual(model)
            check(model.requiredCampusVerificationState == "unconfirmed" && model.readiness.tone != "success"
                  && model.memoryVerificationMessage == SharedCopy.t("campus.verify.failed"),
                  "Explicit service verification cannot turn conflicting current candidates green even when both pages respond")
            await service.configure(replies: [both], reports: [online])
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 2 && model.requiredCampusVerificationState == "confirmed",
                  "A later online round independently verifies again rather than borrowing rejected conflicting proof")
            check(await service.snapshot().submissions == 0 && !model.currentNetworkAuthorized,
                  "Conflicting-hint service checks never submit credentials or grant trust")
        }

        // Explicit service confirmation and the automatic online path share the
        // same attachment/region context, while a region edit invalidates it.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            model.verifyCurrentCampusService(); try await finishManual(model)
            let explicitSnapshot = await service.snapshot()
            check(model.requiredCampusVerificationState == "confirmed" && explicitSnapshot.discoveries.count == 1,
                  "Explicit verification supplies a confirmed service context")
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 1 && model.readiness.tone == "success",
                  "Automatic online monitoring reuses the same explicit service context without a duplicate discovery")
            model.area = .dormitory
            check(model.requiredCampusVerificationState == "notChecked",
                  "Changing the selected service area clears displayed proof from the previous context")
            await service.configure(replies: [CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)])
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 2 && model.requiredCampusVerificationState == "unconfirmed"
                  && model.readiness.tone != "success", "The changed area is rechecked and cannot borrow the old green proof")
            check(await service.snapshot().submissions == 0, "Region rechecks remain entirely credential-free")
        }

        // Ordinary detection and unknown networks cannot acquire campus eligibility.
        for scope in ["manual", "observe", "readOnly", "unremembered"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            if scope != "unremembered" { _ = try seed(root) }
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let mode: MonitoringMode = scope == "manual" ? .manual : scope == "observe" ? .observe : .recover
            let model = fixture(root, service, clock, mode: mode, readOnly: scope == "readOnly")
            model.checkConnection(); try await finishManual(model)
            if scope != "manual" { await model.runAutomaticCycle() }
            let excludedSnapshot = await service.snapshot()
            check(excludedSnapshot.discoveries.isEmpty && excludedSnapshot.submissions == 0,
                  "\(scope) mode retains check-only behavior without automatic campus discovery")
        }

        // The old request deliberately returns after revocation/change/cancellation.
        for action in ["cancel", "path", "forget", "forgetSelected", "forgetAll", "configuration", "sleep", "mode", "manualMode", "revoke", "maintenance"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            await service.configure(replies: [both], gated: true)
            let task = Task { await model.runAutomaticCycle() }
            try await waitUntil("gated \(action) confirmation") { await service.isWaiting() }
            switch action {
            case "cancel": model.cancelCurrentOperation()
            case "path": attach(model, lease: "B")
            case "forget": model.forgetCurrentNetwork()
            case "forgetSelected": model.forgetNetwork(id: recordID)
            case "forgetAll": model.forgetAllNetworks()
            case "configuration": model.area = .dormitory
            case "sleep": model.setSleeping(true)
            case "mode": model.setMonitoringMode(.observe)
            case "manualMode": model.setMonitoringMode(.manual)
            case "maintenance": _ = await model.prepareForAppMaintenance()
            default: model.revokeCurrentNetworkAuthorization()
            }
            let afterAction = model.memoryVerificationMessage
            check(afterAction != SharedCopy.t("campus.verify.checking") && model.engineStatus != "discovering",
                  "\(action) leaves neither obsolete checking feedback nor a discovering engine after stopping")
            if action == "path" {
                check(!model.isOnline && model.presentationStatus != "online",
                      "A new physical path cannot display the old attachment's online result before its own check")
            }
            await service.release(); await task.value
            check(model.requiredCampusVerificationState != "confirmed" && model.memoryVerificationMessage == afterAction
                  && model.engineStatus != "discovering",
                  "Late \(action) result cannot install stale campus proof or overwrite current feedback")
            if action == "path" {
                check(!model.isOnline && model.presentationStatus != "online",
                      "A late old-path result cannot restore online presentation on an unchecked new attachment")
            }
            check(await service.snapshot().submissions == 0 && !model.currentNetworkAuthorized,
                  "\(action) during check-only confirmation sends no credentials and grants no trust")
        }

        // A check-only manual operation may contain the automatic confirmation
        // phase. Disabling recovery cancels that phase, not an explicit service action.
        for destination in [MonitoringMode.manual, .observe] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            await service.configure(replies: [both], gated: true)
            model.checkConnection()
            try await waitUntil("manual check's automatic confirmation") { await service.isWaiting() }
            check(model.campusVerificationInProgress, "A manual connectivity check can expose its real automatic confirmation phase")
            model.setMonitoringMode(destination)
            check(!model.busy && !model.campusVerificationInProgress && model.engineStatus != "discovering"
                  && model.memoryVerificationMessage != SharedCopy.t("campus.verify.checking"),
                  "Switching a manual check to \(destination.rawValue) stops its automatic confirmation feedback")
            let feedback = model.memoryVerificationMessage
            await service.release()
            try await waitUntil("cancelled manual check service completion") { await service.snapshot().completed == 1 }
            // Yield main-actor execution to the now-released, cancelled manual worker.
            await Task.yield()
            check(model.requiredCampusVerificationState != "confirmed" && model.memoryVerificationMessage == feedback
                  && model.engineStatus != "discovering",
                  "The manual check's late result cannot restore confirmation after selecting \(destination.rawValue)")
            check(await service.snapshot().submissions == 0, "Mode changes during the automatic confirmation phase send no credentials")
        }

        // Credential/user/security holds remain independent of harmless service proof.
        for reason in [HoldReason.userPaused, .security, .credentials] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let hold = AuthenticationHold(reason: reason, origin: reason == .userPaused ? .user : .global)
            let journal = RecoveryJournal(generation: 1, holds: [hold], notBefore: date.addingTimeInterval(60), pendingSubmission: false)
            try journal.write(root.appendingPathComponent("recovery.json"))
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            let holds = model.holds
            let cooldown = model.cooldownRemaining
            await model.runAutomaticCycle()
            check(model.holds == holds && model.cooldownRemaining == cooldown,
                  "Campus proof cannot clear \(reason.key) or shorten the existing cooldown")
            check(await service.snapshot().submissions == 0,
                  "Campus confirmation under \(reason.key) remains credential-free")
        }

        // Displayed positive proof cannot substitute for the recovery round's fresh discovery.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            await model.runAutomaticCycle()
            await service.configure(replies: [both], reports: [offline, offline, online])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            let recoverySnapshot = await service.snapshot()
            check(recoverySnapshot.discoveries.count == 2 && recoverySnapshot.submissions == 1,
                  "A later genuine outage still requires fresh campus discovery before one simulated login")
        }
        // Periodic confirmation is bounded by a completion-based monotonic clock.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let bytes = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock)
            await model.runAutomaticCycle()
            clock.date.addTimeInterval(86_400) // Wall clock changes cannot expire confirmation.
            clock.uptime += 899
            await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 1, "Wall clock jumps do not replace the monotonic 15 minute deadline")
            clock.uptime += 1
            await service.configure(replies: [failed], gated: true)
            let pending = Task { await model.runAutomaticCycle() }
            try await waitUntil("periodic recheck") { await service.isWaiting() }
            check(model.campusVerificationInProgress && model.requiredCampusVerificationState != "confirmed",
                  "A new periodic discovery clears the previous success while its result is unknown")
            clock.uptime += 60
            await service.release(); await pending.value
            check(model.isOnline && model.campusVerificationFailed, "A failed refresh preserves public online but removes old green proof")
            await service.configure(replies: [both])
            clock.uptime += 29; await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 2, "Retry remains ineligible for 29 seconds after completion, not request start")
            clock.uptime += 1; await model.runAutomaticCycle()
            check(await service.snapshot().discoveries.count == 3 && model.requiredCampusVerificationState == "confirmed",
                  "Monitoring can confirm recovery after the completion-based retry deadline")
            check(await service.snapshot().submissions == 0 && !model.currentNetworkAuthorized,
                  "Periodic refresh and recovery grant no trust and submit no credentials")
            check(try Data(contentsOf: root.appendingPathComponent("network-memory.json")) == bytes,
                  "Periodic refresh never extends or rewrites stored memory proof")
        }
        // An explicit verification takes private fresh hints, not the old visible report.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root)
            let service = AutomaticCampusService(), clock = AutomaticCampusClock()
            let model = fixture(root, service, clock, area: .automatic)
            let old = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .redirect, elapsed: 0.01, campusAreaHint: .dormitory), mode: .complete)
            await service.configure(reports: [old])
            model.checkConnection(); try await finishManual(model)
            let checked = model.lastCheckedAt
            let fresh = ConnectivityReport(appleResult: .init(state: .redirect, elapsed: 0.01, campusAreaHint: .teaching),
                microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick)
            await service.configure(replies: [CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)], reports: [fresh])
            model.verifyCurrentCampusService(); try await finishManual(model)
            check(model.requiredCampusVerificationState == "confirmed" && !model.needsDormitoryRecoveryEligibility,
                  "Private current teaching hints replace stale dormitory hints for service qualification")
            check(model.lastCheckedAt == checked && model.lastInternetCheck?.base.microsoftResult.campusAreaHint == .dormitory,
                  "Private hint sampling preserves the visible report and its timestamp")
            check(await service.snapshot().submissions == 0, "Private hint collection and service verification remain credential-free")
        }
        print("Automatic campus verification: \(assertions) assertions passed; fake services and virtual clock only.")
    }
}
