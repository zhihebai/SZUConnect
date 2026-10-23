import Foundation

/// Deliberately has no URLSession, socket, Keychain or system-service dependencies.
private actor FakeConnectionService: ConnectionService {
    enum Stage: Hashable, Sendable { case probe, confirmation, login }
    enum LoginReply: Sendable {
        case accepted
        case rejected(PortalError)
        case unsafeTransportError
    }
    struct Snapshot: Sendable {
        let probes: Int
        let confirmations: Int
        let submitted: [Credentials]
        let areas: [CampusArea]
    }
    private var reports: [ConnectivityReport] = []
    private var fallbackReport = ConnectivityReport(apple: false, microsoft: false)
    private var campus: CampusArea? = .teaching
    private var reply: LoginReply = .accepted
    private var probes = 0
    private var confirmations = 0
    private var submitted: [Credentials] = []
    private var areas: [CampusArea] = []
    private var paused: Set<Stage> = []
    private var waiting: [Stage: CheckedContinuation<Void, Never>] = [:]

    func configure(reports: [ConnectivityReport] = [], campus: CampusArea? = .teaching,
                   reply: LoginReply = .accepted) {
        self.reports = reports
        self.campus = campus
        self.reply = reply
    }
    func pauseNext(_ stage: Stage) { paused.insert(stage) }
    func release(_ stage: Stage) {
        guard let continuation = waiting.removeValue(forKey: stage) else {
            preconditionFailure("No paused fake operation to release")
        }
        continuation.resume()
    }
    func isWaiting(_ stage: Stage) -> Bool { waiting[stage] != nil }
    private func waitIfPaused(_ stage: Stage) async {
        guard paused.remove(stage) != nil else { return }
        await withCheckedContinuation { waiting[stage] = $0 }
    }
    func snapshot() -> Snapshot {
        Snapshot(probes: probes, confirmations: confirmations, submitted: submitted, areas: areas)
    }
    func probe() async -> ConnectivityReport {
        probes += 1
        let result = reports.isEmpty ? fallbackReport : reports.removeFirst()
        await waitIfPaused(.probe)
        return result
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        confirmations += 1
        let result = campus
        await waitIfPaused(.confirmation)
        return result
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        submitted.append(credentials)
        areas.append(area)
        let result = reply
        await waitIfPaused(.login)
        switch result {
        case .accepted: return "离线认证已接受"
        case .rejected(let error): throw error
        case .unsafeTransportError:
            throw NSError(domain: "OfflineFixture", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "request https://fixture.invalid/?username=fixture-account&password=fixture-password"
            ])
        }
    }
}

/// Delayed replies deliberately ignore task cancellation to exercise generation fences.
private actor FakeSystemConnectivity {
    private var replies: [ConnectivityReport]
    private var delayed: Set<Int>
    private var waiting: [Int: CheckedContinuation<Void, Never>] = [:]
    private(set) var calls = 0
    private(set) var completed = 0

    init(replies: [ConnectivityReport], delayed: Set<Int> = []) {
        self.replies = replies; self.delayed = delayed
    }
    func probe(_ mode: ConnectivityProbeMode) async -> ConnectivityReport {
        calls += 1
        let call = calls
        let result = replies.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : replies.removeFirst()
        if delayed.contains(call) { await withCheckedContinuation { waiting[call] = $0 } }
        completed += 1
        return ConnectivityReport(appleResult: result.appleResult, microsoftResult: result.microsoftResult, mode: mode)
    }
    func isWaiting(_ call: Int) -> Bool { waiting[call] != nil }
    func release(_ call: Int) { waiting.removeValue(forKey: call)?.resume() }
}

@main
@MainActor
struct AppModelOfflineTests {
    private static var passed = 0
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let online = ConnectivityReport(apple: true, microsoft: false)
    private static let identityA = NetworkIdentity(fingerprint: "offline-network-A")
    private static let identityB = NetworkIdentity(fingerprint: "offline-network-B")
    private static let identityWithAttachment = NetworkIdentity(fingerprint: "offline-network-A-with-attachment",
                                                                 permitsBriefInterruption: true)

    private static func check(_ condition: Bool, _ name: String) {
        guard condition else { fatalError("FAIL: \(name)") }
        passed += 1
        print("PASS: \(name)")
    }
    private static func waitUntil(_ description: String,
                                  _ condition: @MainActor () async -> Bool) async throws {
        let deadline = Date().addingTimeInterval(3)
        while Date() < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(1))
        }
        fatalError("Timed out waiting for \(description)")
    }
    private static func fixture(readOnly: Bool = false, area: CampusArea = .teaching,
                                clock: @escaping () -> Date = Date.init)
      -> (AppModel, FakeConnectionService) {
        let service = FakeConnectionService()
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: clock)
        model.account = "fixture-account"
        model.password = "fixture-password"
        model.area = area
        model.saveCredentials()
        model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA)
        model.setAutoReconnect(true)
        return (model, service)
    }
    private static func finishManual(_ model: AppModel) async throws {
        try await waitUntil("manual completion") { !model.busy }
    }

    static func main() async throws {
        setbuf(stdout, nil)

        do {
            let service = FakeConnectionService()
            let display = FakeSystemConnectivity(replies: [online])
            let model = AppModel(client: service, isolated: true, readOnly: true,
                systemConnectivityProbe: { await display.probe($0) })
            model.receivePath(usable: true, interfaces: "offline-system", identity: identityA)
            model.setMonitoringMode(.manual)
            model.refreshSystemConnectivity()
            try await waitUntil("system display result") { !model.systemConnectivityChecking }
            let calls = await service.snapshot()
            check(model.systemConnectivity?.online == true && !model.isOnline && model.lastInternetCheck == nil,
                  "System availability stays separate from campus connectivity evidence")
            check(!model.currentNetworkAuthorized && calls.probes == 0 && calls.submitted.isEmpty,
                  "Display probes do not invoke the campus client, trust a network or submit credentials")
            check(model.systemPathIsLeading && model.presentation.headline == "env.systemOnline.title",
                  "Public connectivity remains visible before campus account or trust setup")
            let previous = model.systemConnectivityCheckedAt
            model.receivePath(usable: true, interfaces: "offline-system", identity: identityA)
            check(model.systemConnectivityCheckedAt == previous && model.systemPathIsLeading,
                  "Identical path observations preserve a completed display result")
        }
        do {
            let service = FakeConnectionService()
            let display = FakeSystemConnectivity(replies: [online, offline], delayed: [1])
            let model = AppModel(client: service, isolated: true, readOnly: true,
                systemConnectivityProbe: { await display.probe($0) })
            model.receivePath(usable: true, interfaces: "offline-system", identity: identityA)
            model.setMonitoringMode(.manual)
            model.refreshSystemConnectivity()
            try await waitUntil("first delayed display") { await display.isWaiting(1) }
            model.refreshSystemConnectivity()
            try await waitUntil("new display result") { !model.systemConnectivityChecking }
            await display.release(1)
            try await waitUntil("stale display returned") { await display.completed == 2 }
            for _ in 0..<10 { await Task.yield() }
            check(model.systemConnectivity?.online == false,
                  "A superseded success cannot overwrite the newer system display result")
        }
        do {
            let service = FakeConnectionService()
            let display = FakeSystemConnectivity(replies: [online, offline], delayed: [1])
            let model = AppModel(client: service, isolated: true, readOnly: true,
                systemConnectivityProbe: { await display.probe($0) })
            model.setMonitoringMode(.observe)
            model.receivePath(usable: true, interfaces: "offline-system", identity: identityA)
            model.refreshSystemConnectivity()
            try await waitUntil("display before automatic path refresh") { await display.isWaiting(1) }
            model.receivePath(usable: true, interfaces: "offline-system-B", identity: identityB)
            try await waitUntil("display rechecked for new path") { await display.calls == 2 && !model.systemConnectivityChecking }
            await display.release(1)
            try await waitUntil("old automatic path result returned") { await display.completed == 2 }
            for _ in 0..<10 { await Task.yield() }
            check(model.systemConnectivity?.online == false && !model.currentNetworkAuthorized,
                  "Automatic monitoring replaces display evidence after a path change without borrowing old success or trust")
        }
        for interruption in ["network", "cancel", "sleep"] {
            let service = FakeConnectionService()
            let display = FakeSystemConnectivity(replies: [online], delayed: [1])
            let model = AppModel(client: service, isolated: true, readOnly: true,
                systemConnectivityProbe: { await display.probe($0) })
            model.receivePath(usable: true, interfaces: "offline-system", identity: identityA)
            model.setMonitoringMode(.manual)
            model.refreshSystemConnectivity()
            try await waitUntil("display before interruption") { await display.isWaiting(1) }
            if interruption == "network" { model.receivePath(usable: true, interfaces: "offline-system-B", identity: identityB) }
            else if interruption == "sleep" { model.setSleeping(true) }
            else { model.cancelCurrentOperation() }
            await display.release(1)
            try await waitUntil("invalidated display returned") { await display.completed == 1 }
            for _ in 0..<10 { await Task.yield() }
            check(model.systemConnectivity == nil && !model.systemConnectivityChecking,
                  "\(interruption) invalidates system evidence and rejects late completion")
            check((await service.snapshot()).submitted.isEmpty,
                  "\(interruption) does not initiate campus authentication")
        }
        for error in [PortalError.rejected, .authenticationUncertain, .tls] {
            let service = FakeConnectionService()
            let display = FakeSystemConnectivity(replies: [online])
            let model = AppModel(client: service, isolated: true,
                systemConnectivityProbe: { await display.probe($0) })
            model.account = "fixture-account"; model.password = "fixture-password"
            model.area = .teaching; model.saveCredentials()
            model.receivePath(usable: true, interfaces: "offline-system", identity: identityA)
            model.setMonitoringMode(.manual)
            await service.configure(reply: .rejected(error))
            model.connect()
            try await finishManual(model)
            try await waitUntil("protected display completion") { !model.systemConnectivityChecking }
            check(model.systemConnectivity?.online == true && !model.systemPathCanLead,
                  "System success cannot cover a rejected, uncertain or TLS-protected authentication state")
            check(model.presentation.headline != "env.systemOnline.title",
                  "Authentication protection remains the primary visible message")
        }

        do {
            let (model, service) = fixture()
            model.authorizeCurrentNetwork()
            model.setProbeMode(.complete)
            check(model.currentNetworkAuthorized && model.probeMode == .complete,
                  "Choosing complete checks does not revoke network trust or submit credentials")
            // Two partial rounds may recover once, followed
            // by three read-only confirmations; persistent partial results never loop submissions.
            await service.configure(reports: Array(repeating: online, count: 6))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(model.isOnline && model.lastProbeMode == .complete && model.lastProbePassedCount == 1,
                  "Partial complete checks preserve basic connectivity and expose their stricter result")
            check(await service.snapshot().submitted.count == 1,
                  "Persistent partial complete results permit one bounded recovery without repeated submissions")
        }

        // A visible user command must not disappear behind a quiet background probe.
        do {
            let (model, service) = fixture()
            await service.configure(reports: [online, offline, online])
            await model.runAutomaticCycle()
            await service.pauseNext(.probe)
            let background = Task { await model.runAutomaticCycle() }
            try await waitUntil("background probe before manual priority") { await service.isWaiting(.probe) }
            model.checkConnection()
            check(model.isForegroundBusy, "Manual detection takes ownership from a quiet automatic probe")
            try await finishManual(model)
            let manualSnapshot = await service.snapshot()
            check(model.isOnline && manualSnapshot.submitted.isEmpty,
                  "Priority detection uses no credentials and reports its own result")
            let latest = model.lastCheckedAt
            await service.release(.probe)
            await background.value
            check(model.isOnline && model.lastCheckedAt == latest && !model.busy,
                  "Cancelled background result cannot overwrite a newer manual success")
        }

        do {
            let (model, service) = fixture()
            await service.configure(reports: [online, offline])
            await model.runAutomaticCycle()
            await service.pauseNext(.probe)
            let background = Task { await model.runAutomaticCycle() }
            try await waitUntil("background probe before manual login") { await service.isWaiting(.probe) }
            await service.configure(reports: [offline, online], reply: .accepted)
            model.connect()
            try await finishManual(model)
            check(await service.snapshot().submitted.count == 1,
                  "A priority manual login submits exactly one explicit request")
            await service.release(.probe)
            await background.value
            check(await service.snapshot().confirmations == 0,
                  "Late background failure never starts a second identity/authentication path")
            check(model.isOnline && !model.busy, "The manual login result survives the cancelled background round")
        }

        // Network permission belongs to the observed network, not an individual round or draft.
        do {
            let (model, service) = fixture(area: .dormitory)
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized, "Explicit permission is granted before cancellation regression")
            model.cancelCurrentOperation()
            check(model.currentNetworkAuthorized, "Cancelling a round preserves current network permission")
            model.account = "edited-draft-account"
            check(model.currentNetworkAuthorized, "Editing an account draft preserves current network permission")
            model.password = "edited-draft-password"
            model.area = .teaching
            model.saveCredentials()
            check(model.currentNetworkAuthorized, "Editing area, password and saving preserves network permission")
            for mode: MonitoringMode in [.observe, .manual, .recover] {
                model.setMonitoringMode(mode)
                check(model.currentNetworkAuthorized, "Switching to \(mode.rawValue) preserves network permission")
            }
            model.resumeAutomaticRecovery()
            check(model.currentNetworkAuthorized, "Explicitly resuming checks preserves network permission")
            let snapshot = await service.snapshot()
            check(snapshot.probes == 0 && snapshot.confirmations == 0 && snapshot.submitted.isEmpty,
                  "Granting permission, cancelling and editing never starts a network request")
        }

        // Repeated OS metadata updates of the same network are not network replacements.
        for identity in [identityA, identityWithAttachment] {
            let (model, service) = fixture(area: .dormitory)
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identity)
            model.authorizeCurrentNetwork()
            await service.configure(reports: [online])
            await model.runAutomaticCycle()
            let lastChecked = model.lastCheckedAt
            let probeSummary = model.probeSummary
            let events = model.events.count
            let nextCheck = model.nextCheckAt
            for _ in 0..<3 {
                model.receivePath(usable: true, interfaces: "renamed-display-only", identity: identity)
            }
            check(model.currentNetworkAuthorized && model.isOnline && model.lastCheckedAt == lastChecked,
                  "Same-identity path updates preserve permission and last confirmed connectivity")
            check(model.probeSummary == probeSummary && model.events.count == events
                  && model.nextCheckAt == nextCheck,
                  "Same-identity updates preserve completed evidence without adding events or resetting schedule")
            model.receivePath(usable: true, interfaces: "renamed-display-only", constrained: true, identity: identity)
            check(model.currentNetworkAuthorized && model.isOnline && model.recommendedCheckDelay == 120,
                  "Constrained-only metadata update preserves network permission and adopts economical cadence")
            check(await service.snapshot().probes == 1, "Metadata-only updates never trigger requests in the isolated model")
        }

        // Permission remains a pure action even while a cancellable request owns the model.
        do {
            let (model, service) = fixture(area: .dormitory)
            await service.configure(reports: [online])
            await service.pauseNext(.probe)
            let task = Task { await model.runAutomaticCycle() }
            try await waitUntil("busy permission grant") { await service.isWaiting(.probe) }
            model.authorizeCurrentNetwork()
            let during = await service.snapshot()
            check(model.currentNetworkAuthorized && model.busy && during.probes == 1
                  && during.confirmations == 0 && during.submitted.isEmpty,
                  "Granting network permission while busy neither cancels ownership nor submits a request")
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA)
            check(model.currentNetworkAuthorized && model.busy,
                  "Same-identity notification preserves the in-flight probe owner")
            await service.release(.probe)
            await task.value
            check(model.isOnline && !model.busy && model.currentNetworkAuthorized,
                  "Preserved same-network probe can complete and publish its result")
        }

        // A real trust boundary invalidates old permission and explains why it changed.
        for boundary in ["replacement", "unknown", "link-down", "sleep"] {
            let (model, service) = fixture(area: .dormitory)
            model.authorizeCurrentNetwork()
            switch boundary {
            case "replacement":
                model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityB)
            case "unknown":
                model.receivePath(usable: true, interfaces: "offline-fixture")
            case "link-down":
                model.receivePath(usable: false, interfaces: "offline-fixture", identity: identityA, linkActive: false)
            default:
                model.setSleeping(true)
                model.setSleeping(false)
            }
            check(!model.currentNetworkAuthorized && !model.authorizationReason.isEmpty,
                  "\(boundary) revokes permission with an explanation")
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA)
            check(!model.currentNetworkAuthorized, "Recovery after \(boundary) never silently grants permission again")
            check(await service.snapshot().submitted.isEmpty, "Trust-boundary transition \(boundary) never submits credentials")
        }
        do {
            let (model, service) = fixture(area: .dormitory)
            model.authorizeCurrentNetwork()
            model.setSleeping(true)
            model.setSleeping(false)
            model.authorizeCurrentNetwork()
            check(!model.currentNetworkAuthorized,
                  "Wake cannot reauthorize using the pre-sleep identity without a new path snapshot")
            await model.runAutomaticCycle()
            model.checkConnection()
            model.connect()
            let beforeSnapshot = await service.snapshot()
            check(beforeSnapshot.probes == 0 && beforeSnapshot.confirmations == 0
                  && beforeSnapshot.submitted.isEmpty && !model.busy,
                  "Wake without a fresh path snapshot blocks every probe and credential entry point")
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA)
            check(!model.currentNetworkAuthorized,
                  "Fresh post-wake identity does not restore the pre-sleep permission")
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized,
                  "Explicit permission succeeds after a fresh post-wake identity is observed")
            let afterSnapshot = await service.snapshot()
            check(afterSnapshot.probes == 0 && afterSnapshot.confirmations == 0 && afterSnapshot.submitted.isEmpty,
                  "Post-wake network sampling and permission remain transport-free in the isolated model")
        }
        do {
            let (model, service) = fixture(area: .dormitory)
            model.receivePath(usable: true, interfaces: "offline-fixture")
            model.authorizeCurrentNetwork()
            check(!model.currentNetworkAuthorized && !model.authorizationReason.isEmpty,
                  "Unknown continuity identity cannot receive network permission")
            let snapshot = await service.snapshot()
            check(snapshot.probes == 0 && snapshot.confirmations == 0 && snapshot.submitted.isEmpty,
                  "Declined permission request has no transport side effects")
        }
        do {
            let (model, service) = fixture(area: .dormitory)
            model.receivePath(usable: true, interfaces: "offline-fixture")
            await service.configure(reports: [offline, online], campus: .dormitory)
            model.connect()
            try await finishManual(model)
            let snapshot = await service.snapshot()
            check(model.isOnline && snapshot.submitted.count == 1,
                  "Explicit manual authentication may complete on a usable unknown-identity network")
            check(!model.currentNetworkAuthorized,
                  "Manual success cannot grant ongoing permission when network identity is unknown")
        }

        for stage in [FakeConnectionService.Stage.confirmation, .login] {
            let (model, service) = fixture(area: .dormitory)
            model.authorizeCurrentNetwork()
            await service.configure(campus: .dormitory, reply: .rejected(.network))
            await model.runAutomaticCycle()
            await service.pauseNext(stage)
            let task = Task { await model.runAutomaticCycle() }
            try await waitUntil("same-network \(stage)") { await service.isWaiting(stage) }
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA)
            check(model.currentNetworkAuthorized && model.busy,
                  "Same-identity notification preserves an active \(stage) owner")
            await service.release(stage)
            await task.value
            check(await service.snapshot().submitted.count == 1 && !model.busy && model.currentNetworkAuthorized,
                  "Same-network \(stage) completion remains owned and preserves permission")
        }

        // Short unusable-path bursts with unchanged attachment pause traffic without losing intent.
        let continuityEpoch = Date(timeIntervalSince1970: 2_000_000)
        for duration: TimeInterval in [1, 15, 16] {
            let (model, service) = fixture(area: .dormitory)
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityWithAttachment)
            model.authorizeCurrentNetwork()
            model.receivePath(usable: false, interfaces: "offline-fixture", identity: identityWithAttachment,
                              linkActive: true, now: continuityEpoch)
            await model.runAutomaticCycle()
            model.checkConnection()
            check(await service.snapshot().probes == 0 && !model.busy,
                  "Unusable path blocks all probes during a \(Int(duration))-second continuity pause")
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityWithAttachment,
                              linkActive: true, now: continuityEpoch.addingTimeInterval(duration))
            check(model.currentNetworkAuthorized == (duration <= 15),
                  "Same-attachment pause of \(Int(duration)) seconds honors the 15-second trust limit")
            if duration > 15 {
                check(!model.authorizationReason.isEmpty, "Expired continuity grace explains permission revocation")
            }
        }
        do {
            let (model, service) = fixture(area: .dormitory)
            model.authorizeCurrentNetwork()
            model.receivePath(usable: false, interfaces: "offline-fixture", identity: identityA,
                              linkActive: true, now: continuityEpoch)
            check(!model.currentNetworkAuthorized && !model.authorizationReason.isEmpty,
                  "Weak network identity revokes permission immediately across an unusable path")
            await model.runAutomaticCycle()
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA,
                              linkActive: true, now: continuityEpoch.addingTimeInterval(1))
            check(!model.currentNetworkAuthorized,
                  "Weak identity cannot carry permission across even a one-second interruption")
            check(await service.snapshot().probes == 0,
                  "Weak-identity interruption and recovery do not probe while unavailable")
        }

        // Explicit revocation cancels all previously owned stages, including uncancellable transports.
        for stage in [FakeConnectionService.Stage.probe, .confirmation, .login] {
            let (model, service) = fixture(area: .dormitory)
            model.authorizeCurrentNetwork()
            await service.configure(campus: .dormitory, reply: .accepted)
            if stage != .probe { await model.runAutomaticCycle() }
            await service.pauseNext(stage)
            let task = Task { await model.runAutomaticCycle() }
            try await waitUntil("explicit revoke during \(stage)") { await service.isWaiting(stage) }
            model.revokeCurrentNetworkAuthorization()
            let status = model.status
            let events = model.events.count
            check(!model.currentNetworkAuthorized && !model.busy && !model.authorizationReason.isEmpty,
                  "Explicit revocation clears permission and \(stage) ownership immediately")
            await service.release(stage)
            await task.value
            let snapshot = await service.snapshot()
            check(!model.isOnline && !model.currentNetworkAuthorized && !model.busy
                  && model.status == status && model.events.count == events,
                  "Revoked \(stage) completion cannot restore permission or publish stale state")
            check(stage == .login ? snapshot.submitted.count == 1 : snapshot.submitted.isEmpty,
                  "Revoked \(stage) cannot submit new credentials")
        }

        // A single round may contain several endpoint results, but admits no authentication.
        do {
            let (model, service) = fixture()
            await service.configure(reply: .rejected(.network))
            await model.runAutomaticCycle()
            var snapshot = await service.snapshot()
            check(snapshot.probes == 1 && snapshot.confirmations == 0 && snapshot.submitted.isEmpty,
                  "First failed automatic round only probes")
            check(model.status == SharedCopy.t("status.confirming.headline") && !model.busy,
                  "First failed round releases ownership and requests confirmation")
            await model.runAutomaticCycle()
            snapshot = await service.snapshot()
            check(snapshot.probes == 2 && snapshot.confirmations == 1 && snapshot.submitted.count == 1,
                  "Two failed automatic rounds authenticate exactly once")
            check(snapshot.areas == [.teaching], "Automatic authentication uses confirmed campus area")
            check(!model.automaticRecoveryPaused, "Transient network failure permits later recovery")
        }

        // Probe success breaks a run of failures; manual detection never contributes evidence.
        do {
            let (model, service) = fixture()
            await service.configure(reports: [offline, online, offline, offline], reply: .rejected(.network))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(model.isOnline, "Either successful endpoint marks network online")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.isEmpty, "Success resets consecutive automatic failure evidence")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 1, "Two fresh failures after success can authenticate")
        }
        do {
            let (model, service) = fixture()
            await service.configure(reply: .rejected(.network))
            for _ in 0..<3 {
                model.checkConnection()
                try await finishManual(model)
            }
            var snapshot = await service.snapshot()
            check(snapshot.probes == 3 && snapshot.confirmations == 0 && snapshot.submitted.isEmpty,
                  "Repeated manual checks never discover or submit authentication")
            check(model.recommendedCheckDelay == 30, "Manual failures do not increase automatic backoff")
            await model.runAutomaticCycle()
            snapshot = await service.snapshot()
            check(snapshot.submitted.isEmpty && snapshot.confirmations == 0,
                  "Manual failures do not satisfy automatic authentication threshold")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 1, "Two automatic failures still required after manual checks")
        }

        // Persistent rejections survive ordinary lifecycle changes and explicit saves.
        for reason: PortalError in [.rejected, .invalidCredentials, .tls, .forbiddenTarget, .protocolConflict, .authenticationUncertain] {
            var time = Date()
            let (model, service) = fixture(clock: { time })
            await service.configure(reply: .rejected(reason))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(model.automaticRecoveryPaused, "\(reason) pauses automatic credential submission")
            let before = await service.snapshot()
            model.setAutoReconnect(false)
            model.setAutoReconnect(true)
            model.receivePath(usable: true, interfaces: "replacement-fixture", identity: identityB)
            model.setSleeping(true)
            model.setSleeping(false)
            model.receivePath(usable: true, interfaces: "replacement-fixture", identity: identityB)
            model.saveCredentials()
            for _ in 0..<3 { await model.runAutomaticCycle() }
            let after = await service.snapshot()
            check(after.probes == before.probes + 3 && after.submitted.count == 1,
                  "\(reason) continues read-only probes across toggle, path, sleep and save")
            check(model.automaticRecoveryPaused, "\(reason) block remains visible after lifecycle changes")
            model.resumeAutomaticRecovery()
            check(!model.automaticRecoveryPaused, "Explicit resume releases \(reason) block")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 1, "Resume discards old failure evidence for \(reason)")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 1,
                  "Resume cannot bypass the pending cooldown for \(reason)")
            time = time.addingTimeInterval(91)
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 2,
                  "Fresh evidence after cooldown permits \(reason) retry")
        }

        // Accepted authentication gets bounded read-only verification and a propagation grace period.
        do {
            let (model, service) = fixture()
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            var snapshot = await service.snapshot()
            check(snapshot.submitted.count == 1 && snapshot.probes == 5,
                  "Accepted authentication performs at most three read-only confirmations")
            check(model.status == SharedCopy.t("status.accepted.headline") && !model.isOnline, "Authentication acceptance alone never claims internet access")
            for _ in 0..<3 { await model.runAutomaticCycle() }
            snapshot = await service.snapshot()
            check(snapshot.probes == 8 && snapshot.submitted.count == 1 && snapshot.confirmations == 1,
                  "Grace period keeps probing without another discovery or login")
            check(model.engineStatus == "coolingDown" && model.authenticationSummary == SharedCopy.t("auto.coolingDown.detail")
                  && model.recommendedCheckDelay == 15,
                  "Grace period is visible and schedules read-only confirmations")
        }
        do {
            let (model, service) = fixture()
            await service.configure(reports: [offline, offline, offline, online])
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(snapshot.submitted.count == 1 && snapshot.probes == 4 && model.isOnline,
                  "Post-authentication confirmation stops at first success")
        }

        // Every user entry point honors a read-only launch, including manual connect.
        do {
            let (model, service) = fixture(readOnly: true)
            check(!model.hasSavedCredentials && model.hasCompleteDraft,
                  "Read-only save leaves edited credentials in RAM without replacing the saved account")
            for _ in 0..<3 { await model.runAutomaticCycle() }
            model.connect()
            try await finishManual(model)
            model.checkConnection()
            try await finishManual(model)
            model.authorizeCurrentNetwork()
            check(!model.currentNetworkAuthorized && !model.authorizationReason.isEmpty,
                  "Read-only launch cannot grant current-network permission")
            model.resumeAutomaticRecovery()
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(snapshot.probes == 6 && snapshot.confirmations == 0 && snapshot.submitted.isEmpty,
                  "Read-only launch prevents login through all entry points")
            check(model.sessionReadOnly && model.recoverySummary.contains("仅检测"), "Read-only restriction is visible")
        }

        // An edited credential draft cannot become a background submission.
        do {
            let (model, service) = fixture()
            await service.configure(reply: .rejected(.network))
            model.account = "draft-account"
            model.password = "draft-password"
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            var snapshot = await service.snapshot()
            check(snapshot.submitted.count == 1 && snapshot.submitted[0].account == "fixture-account"
                  && snapshot.submitted[0].password == "fixture-password",
                  "Automatic recovery uses saved credentials while draft is being edited")
            model.connect()
            try await finishManual(model)
            snapshot = await service.snapshot()
            check(snapshot.submitted.count == 2 && snapshot.submitted[1].account == "draft-account"
                  && snapshot.submitted[1].password == "draft-password",
                  "Explicit manual connection uses the current complete draft")
        }

        // A successful unsaved account must not silently re-enable a rejected saved account.
        do {
            let (model, service) = fixture()
            await service.configure(reply: .rejected(.rejected))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(model.automaticRecoveryPaused, "Saved account rejection blocks background submission")
            model.account = "different-unsaved-account"
            model.password = "different-unsaved-password"
            await service.configure(reports: [offline, online])
            model.connect()
            try await finishManual(model)
            let snapshot = await service.snapshot()
            check(snapshot.submitted.count == 2 && snapshot.submitted[1].account == "different-unsaved-account"
                  && model.isOnline, "Manual alternate account can establish connectivity without saving it")
            check(model.automaticRecoveryPaused,
                  "Unsaved alternate account success preserves rejection protection of the saved account")
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 2,
                  "Background recovery cannot reuse rejected saved account after unsaved alternate success")
        }

        // Dormitory automatic authentication needs explicit authorization of this exact path.
        do {
            let (model, service) = fixture(area: .dormitory)
            await service.configure(campus: .dormitory, reply: .rejected(.network))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.isEmpty, "Dormitory discovery alone never authorizes credential submission")
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized, "Explicit action authorizes current usable path")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 1, "Authorized dormitory path permits guarded recovery")
            model.receivePath(usable: true, interfaces: "same-interface-new-network", identity: identityB)
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            let afterPathChange = await service.snapshot()
            check(!model.currentNetworkAuthorized && afterPathChange.submitted.count == 1,
                  "Path replacement revokes dormitory permission even when still usable")
        }
        do {
            let (model, service) = fixture()
            await service.configure(campus: nil)
            await model.runAutomaticCycle()
            check(model.recommendedCheckDelay == 5, "First failed automatic round requests one quick confirmation")
            await model.runAutomaticCycle()
            check(model.recommendedCheckDelay == 120, "Two failed rounds increase backoff exactly twice")
            await model.runAutomaticCycle()
            check(model.recommendedCheckDelay == 240, "Third failed round increases backoff only once")
            check(await service.snapshot().submitted.isEmpty, "Unconfirmed campus never receives automatic credentials")
        }

        // Suspended fake operations return after invalidation to simulate uncancellable transports.
        for stage in [FakeConnectionService.Stage.probe, .confirmation, .login] {
            for change in ["configuration", "path", "sleep"] {
                let (model, service) = fixture()
                if stage != .probe { await model.runAutomaticCycle() }
                await service.configure(reports: stage == .probe ? [online] : [], reply: .accepted)
                await service.pauseNext(stage)
                let task = Task { await model.runAutomaticCycle() }
                try await waitUntil("fake \(stage) suspension") { await service.isWaiting(stage) }
                check(model.busy, "\(stage) owns operation before \(change)")
                switch change {
                case "configuration": model.area = .dormitory
                case "path": model.receivePath(usable: true, interfaces: "new-fixture-path", identity: identityB)
                default: model.setSleeping(true)
                }
                let status = model.status
                let events = model.events.count
                let pausedAfterInvalidation = model.automaticRecoveryPaused
                check(pausedAfterInvalidation == (stage == .login),
                      "Only an invalidated submitted login creates uncertainty after \(change)")
                await service.release(stage)
                await task.value
                let snapshot = await service.snapshot()
                check(!model.busy && !model.isOnline && model.status == status,
                      "Stale \(stage) result cannot publish state after \(change)")
                check(model.events.count == events && model.automaticRecoveryPaused == pausedAfterInvalidation,
                      "Stale \(stage) result cannot append success or alter protection after \(change)")
                check(stage == .login ? snapshot.submitted.count == 1 : snapshot.submitted.isEmpty,
                      "Invalidated \(stage) cannot start a subsequent authentication after \(change)")
                if stage == .login {
                    if change == "sleep" {
                        model.setSleeping(false)
                        model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA)
                    }
                    await model.runAutomaticCycle()
                    await model.runAutomaticCycle()
                    check(await service.snapshot().submitted.count == 1 && model.automaticRecoveryPaused
                          && (model.cooldownRemaining ?? 0) > 0,
                          "Cancelled login retains uncertainty and cooldown after \(change) despite an unknown server outcome")
                }
            }
        }

        // An old task's defer must not release a newer operation's busy ownership.
        do {
            let (model, service) = fixture()
            await model.runAutomaticCycle()
            await service.pauseNext(.confirmation)
            let old = Task { await model.runAutomaticCycle() }
            try await waitUntil("old confirmation") { await service.isWaiting(.confirmation) }
            model.receivePath(usable: true, interfaces: "replacement", identity: identityB)
            await service.pauseNext(.probe)
            let replacement = Task { await model.runAutomaticCycle() }
            try await waitUntil("replacement probe") { await service.isWaiting(.probe) }
            await service.release(.confirmation)
            await old.value
            check(model.busy, "Stale task cleanup cannot release replacement operation's busy state")
            await model.runAutomaticCycle()
            check(await service.snapshot().probes == 3, "An active operation rejects overlapping automatic ticks")
            model.checkConnection()
            check(model.isForegroundBusy, "A manual request preempts the read-only replacement probe")
            try await finishManual(model)
            await service.release(.probe)
            await replacement.value
            check(!model.busy, "Replacement operation releases only its own busy state")
        }

        do {
            let (model, service) = fixture()
            await service.configure(reply: .unsafeTransportError)
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            let publicText = ([model.diagnosticSummary, model.detail, model.errorMessage ?? ""]
                              + model.events.map(\.message)).joined(separator: "\n")
            check(!publicText.contains("fixture-account") && !publicText.contains("fixture-password")
                  && !publicText.contains("fixture.invalid") && !publicText.contains("username="),
                  "Logs and diagnostics never expose credential-bearing arbitrary transport errors")
            let before = model.events.map(\.message)
            let failure = SharedCopy.t("portalError.network")
            check(before.filter { $0 == failure }.count == 1,
                  "The first sanitized transport failure has one visible event")
            model.connect()
            try await finishManual(model)
            check(await service.snapshot().submitted.count == 2, "Explicit retry reaches fake service again for deduplication coverage")
            // Manual and automatic preflight may legitimately add different route
            // diagnostics; the repeated failure itself must remain deduplicated.
            check(model.events.filter { $0.message == failure }.count == 1,
                  "Repeated identical failure is deduplicated in visible log")
            check(before.allSatisfy { prior in model.events.contains { $0.message == prior } },
                  "A new route diagnostic preserves the earlier useful events")
        }
        do {
            let (model, service) = fixture()
            model.receivePath(usable: false, interfaces: "")
            await model.runAutomaticCycle()
            model.connect()
            model.checkConnection()
            check(await service.snapshot().probes == 0 && !model.busy,
                  "Unavailable path blocks every probe and authentication entry point")
            model.receivePath(usable: true, interfaces: "restored", identity: identityB)
            model.setSleeping(true)
            await model.runAutomaticCycle()
            model.connect()
            check(await service.snapshot().probes == 0 && !model.busy,
                  "System sleep blocks every operation without changing network settings")
        }

        // Persistent observation mode guards every entry point, including explicit connect.
        do {
            let (model, service) = fixture()
            model.setMonitoringMode(.observe)
            for _ in 0..<3 { await model.runAutomaticCycle() }
            model.connect()
            try await finishManual(model)
            var snapshot = await service.snapshot()
            check(snapshot.probes == 4 && snapshot.submitted.isEmpty && snapshot.confirmations == 0,
                  "Persistent observation probes through every entry point without submitting credentials")
            check(!model.autoReconnect && model.monitoringMode == .observe,
                  "Observation intent is distinct from automatic authentication")
            check(model.health.samples.count == 4 && model.health.successfulCount == 0,
                  "Only completed observations enter bounded health history")
            model.receivePath(usable: true, interfaces: "private-SSID · 192.0.2.1 · Wi-Fi en0", identity: identityA)
            let exported = try JSONSerialization.jsonObject(with: model.diagnosticExportData) as! [String: Any]
            check(exported["mode"] as? String == "observe" && (exported["samples"] as? [Any])?.count == 4,
                  "Diagnostic JSON records mode and observed samples")
            let text = String(data: model.diagnosticExportData, encoding: .utf8)!
            check(!text.contains("fixture-account") && !text.contains("fixture-password")
                  && !text.contains("private-SSID") && !text.contains("192.0.2.1"),
                  "Entire diagnostic export excludes credentials and private interface metadata")
            model.setMonitoringMode(.manual)
            await model.runAutomaticCycle()
            snapshot = await service.snapshot()
            check(snapshot.probes == 4, "Manual mode stops background probing")
        }
        do {
            let (model, service) = fixture()
            await service.pauseNext(.login)
            await model.runAutomaticCycle()
            let inFlight = Task { await model.runAutomaticCycle() }
            try await waitUntil("login before observation") { await service.isWaiting(.login) }
            model.setMonitoringMode(.observe)
            await service.release(.login)
            await inFlight.value
            check(!model.isOnline && !model.busy && model.monitoringMode == .observe,
                  "Switching to observation invalidates an in-flight authentication result")
            await model.runAutomaticCycle()
            check(await service.snapshot().submitted.count == 1,
                  "Observation never repeats an interrupted credential submission")
        }

        do {
            let (model, service) = fixture()
            model.connectEvenIfOnline(confirmation: model.forcedLoginConfirmation())
            check(await service.snapshot().probes == 0,
                  "Force-login entry requires a fresh explicit offer rather than bypassing confirmation")
            await service.configure(reports: [online])
            model.connect()
            try await finishManual(model)
            check(model.offerForcedLogin, "Manual passing probe creates the one-shot offer")
            await service.configure(reports: [offline])
            await model.runAutomaticCycle()
            check(!model.offerForcedLogin, "A later failed probe invalidates the old passing-probe offer")
        }

        // "仍要登录" is an explicit, one-time manual override after a passing check.
        do {
            let (model, service) = fixture()
            await service.configure(reports: [online], reply: .accepted)
            model.checkConnection()
            try await finishManual(model)
            check(model.isOnline && !model.offerForcedLogin, "A read-only check never offers a login override")
            await service.configure(reports: [online], reply: .accepted)
            model.connect()
            try await finishManual(model)
            var snapshot = await service.snapshot()
            check(model.isOnline && model.offerForcedLogin && snapshot.submitted.isEmpty,
                  "Connecting on a passing network submits nothing and offers an explicit override")
            await service.configure(reports: [online, online], reply: .accepted)
            model.connectEvenIfOnline(confirmation: model.forcedLoginConfirmation())
            try await finishManual(model)
            snapshot = await service.snapshot()
            check(snapshot.submitted.count == 1 && model.isOnline && !model.offerForcedLogin,
                  "The explicit override submits the current draft exactly once and clears the offer")
            for _ in 0..<2 { await model.runAutomaticCycle() }
            snapshot = await service.snapshot()
            check(snapshot.submitted.count == 1, "An override never turns into a background submission")
        }
        do {
            let (model, service) = fixture()
            model.setMonitoringMode(.observe)
            await service.configure(reports: [online, online], reply: .accepted)
            model.connect()
            try await finishManual(model)
            model.connectEvenIfOnline(confirmation: model.forcedLoginConfirmation())
            try await finishManual(model)
            let snapshot = await service.snapshot()
            check(snapshot.submitted.isEmpty && snapshot.probes == 1 && !model.offerForcedLogin,
                  "Read-only guard neither offers nor performs a login override")
        }

        // Tunnels are routing context. The same tunnel keeps trust; toggling it revokes with a reason.
        do {
            let (model, service) = fixture(area: .dormitory)
            let tunnelled = NetworkIdentity(fingerprint: "offline-network-A-via-tunnel")
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: tunnelled, tunnel: true)
            check(model.tunnelActive && !model.currentNetworkAuthorized && model.authorizationReason.contains("VPN"),
                  "Turning a tunnel on is a routing change with its own reason")
            model.authorizeCurrentNetwork()
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: tunnelled, tunnel: true)
            check(model.currentNetworkAuthorized, "Repeated notifications under the same tunnel keep trust")
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: identityA, tunnel: false)
            check(!model.currentNetworkAuthorized && !model.tunnelActive && model.authorizationReason.contains("VPN"),
                  "Turning the tunnel off revokes trust and explains why")
            let snapshot = await service.snapshot()
            check(snapshot.probes == 0 && snapshot.submitted.isEmpty, "Tunnel changes never send requests by themselves")
        }

        // Account draft helpers never touch the Keychain or reveal the full account.
        do {
            let (model, _) = fixture()
            check(model.hasSavedCredentials && model.hasCompleteDraft && !model.hasUnsavedCredentialChanges,
                  "A saved account with an identical draft has no pending edits")
            model.password = "edited-password"
            check(model.hasUnsavedCredentialChanges, "Editing the draft is shown as an unsaved change")
            model.revertCredentialDraft()
            check(!model.hasUnsavedCredentialChanges && model.password == "fixture-password",
                  "Reverting restores the saved account")
            check(model.savedAccountHint == AppModel.maskedAccount("fixture-account")
                  && !(model.savedAccountHint ?? "fixture-account").contains("fixture-account"),
                  "The saved account is only ever shown masked")
            check(AppModel.maskedAccount("2023123456") == "2023••••56" && AppModel.maskedAccount("ab") == "a•••",
                  "Masking keeps a recognisable prefix and suffix only")
        }

        // A failed check explains the result without sending credentials.
        do {
            let (model, service) = fixture()
            let captive = ConnectivityReport(appleResult: ConnectivityEndpointResult(state: .tls, elapsed: 0.05),
                                             microsoftResult: ConnectivityEndpointResult(state: .redirect, elapsed: 0.05))
            await service.configure(reports: [captive])
            model.checkConnection()
            try await finishManual(model)
            let snapshot = await service.snapshot()
            check(model.lastDiagnosis == .portal && model.detail == ConnectivityDiagnosis.portal.explanation
                  && !model.isOnline && snapshot.submitted.isEmpty,
                  "A captive redirect is explained in plain language without sending the account")
        }

        // Editing during a manual login stops it without leaving a stale SharedCopy.t("status.loggingIn.headline").
        do {
            let (model, service) = fixture()
            await service.configure(reports: [offline], reply: .accepted)
            await service.pauseNext(.login)
            model.connect()
            try await waitUntil("manual login in flight") { await service.isWaiting(.login) }
            check(model.status == SharedCopy.t("status.loggingIn.headline") && model.isForegroundBusy, "A manual login is visible foreground work")
            model.password = "edited-password"
            check(!model.busy && !model.isForegroundBusy && model.status == "已取消",
                  "Editing the account stops the login without leaving a stale status")
            await service.release(.login)
            try await Task.sleep(for: .milliseconds(30))
            let snapshot = await service.snapshot()
            check(model.status == "已取消" && !model.isOnline && snapshot.submitted.count == 1,
                  "The cancelled login cannot publish its late result")
        }

        // Routine checks of a healthy connection are not presented as foreground work.
        do {
            let (model, service) = fixture()
            await service.configure(reports: [online, online])
            await model.runAutomaticCycle()
            await service.pauseNext(.probe)
            let routine = Task { await model.runAutomaticCycle() }
            try await waitUntil("routine probe") { await service.isWaiting(.probe) }
            check(model.busy && model.isOnline && !model.isForegroundBusy,
                  "Routine checks of a healthy connection stay out of the way")
            await service.release(.probe)
            await routine.value
            check(!model.busy && model.isOnline, "The routine check completes normally")
        }

        // Only a complete passing check settles uncertainty; rejections stay paused.
        do {
            let (model, service) = fixture()
            await service.configure(reply: .rejected(.authenticationUncertain))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            check(model.automaticRecoveryPaused, "An uncertain submission pauses automatic login")
            await service.configure(reports: [online])
            await model.runAutomaticCycle()
            let partialSnapshot = await service.snapshot()
            check(model.automaticRecoveryPaused && partialSnapshot.submitted.count == 1,
                  "One passing endpoint cannot settle uncertainty or trigger another submission")
            await service.configure(reports: [ConnectivityReport(apple: true, microsoft: true)])
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(!model.automaticRecoveryPaused && model.isOnline && snapshot.submitted.count == 1,
                  "A complete passing check settles the uncertain submission without submitting again")
        }
        do {
            let (model, service) = fixture()
            await service.configure(reply: .rejected(.rejected))
            await model.runAutomaticCycle()
            await model.runAutomaticCycle()
            await service.configure(reports: [online])
            await model.runAutomaticCycle()
            check(model.automaticRecoveryPaused && model.isOnline,
                  "A credential rejection stays paused even after the network works")
        }

        print("\(passed) isolated AppModel assertions passed. No real service, app, network, Keychain, monitor or defaults was used.")
    }
}
