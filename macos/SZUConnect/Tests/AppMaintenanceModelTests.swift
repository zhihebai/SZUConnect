import Foundation
import Darwin

/// Values never reach a persistent preference domain, including in failure tests.
private final class MaintenancePreferences: UserDefaults, @unchecked Sendable {
    var values: [String: Any] = [:]
    var writes = 0
    init() { super.init(suiteName: "local.SZUConnect.offline-maintenance.\(UUID().uuidString)")! }
    override func object(forKey key: String) -> Any? { values[key] }
    override func string(forKey key: String) -> String? { values[key] as? String }
    override func bool(forKey key: String) -> Bool { values[key] as? Bool ?? false }
    override func set(_ value: Any?, forKey key: String) { writes += 1; values[key] = value }
    override func set(_ value: Bool, forKey key: String) { writes += 1; values[key] = value }
}

/// Deliberately ignores cancellation until released: cancellation alone is not a drain.
private actor MaintenanceGate {
    private(set) var started = 0
    private(set) var finished = 0
    private(set) var cancelled = 0
    private var pending: [Int: CheckedContinuation<Void, Never>] = [:]
    func enter() async {
        started += 1
        let id = started
        await withTaskCancellationHandler {
            await withCheckedContinuation { pending[id] = $0 }
        } onCancel: {
            Task { await self.noteCancellation() }
        }
        finished += 1
    }
    private func noteCancellation() { cancelled += 1 }
    func release(_ id: Int = 1) { pending.removeValue(forKey: id)?.resume() }
    func releaseAll() { let current = pending.values; pending.removeAll(); for gate in current { gate.resume() } }
}

private actor MaintenanceConnection: ConnectionService {
    let probeGate: MaintenanceGate?
    let loginGate: MaintenanceGate?
    let online: Bool
    private(set) var probes = 0
    private(set) var submissions = 0
    private(set) var resets = 0
    init(probeGate: MaintenanceGate? = nil, loginGate: MaintenanceGate? = nil, online: Bool = true) {
        self.probeGate = probeGate; self.loginGate = loginGate; self.online = online
    }
    func resetConnections() async { resets += 1 }
    func probe() async -> ConnectivityReport {
        probes += 1
        await probeGate?.enter()
        return ConnectivityReport(apple: online, microsoft: online)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { .teaching }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("Use fake classified login") }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        submissions += 1
        await loginGate?.enter()
        return LoginAttempt(outcome: .success, message: "offline-late-success")
    }
}

private actor MaintenanceWiFi: WiFiRecoveryServicing {
    let gate: MaintenanceGate
    private(set) var token: WiFiRecoveryAuthorization?
    init(_ gate: MaintenanceGate) { self.gate = gate }
    func recover(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) async -> WiFiRecoveryResult {
        token = authorization
        await gate.enter()
        return .joined
    }
    var authorized: Bool { token?.isValid == true }
}

@MainActor private final class MaintenanceFixture {
    let root: URL
    let supportParent: URL
    let support: URL
    let preferences = MaintenancePreferences()
    let credentials = Credentials(account: "fixture-maintenance-account", password: "fixture-maintenance-password")
    init() throws {
        root = FileManager.default.temporaryDirectory.resolvingSymlinksInPath()
            .appendingPathComponent("szu-maintenance-" + UUID().uuidString, isDirectory: true)
        supportParent = root.appendingPathComponent("Application Support", isDirectory: true)
        support = supportParent.appendingPathComponent("SZUConnect", isDirectory: true)
        try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        try NetworkMemoryFile.write(NetworkMemory(salt: "00112233445566778899aabbccddeeff").encoded(),
            to: support.appendingPathComponent("network-memory.json"))
        try NetworkLabels().encoded().write(to: support.appendingPathComponent(NetworkLabelsStore.filename))
    }
    func clean() { try? FileManager.default.removeItem(at: root) }
    func files() throws -> [String: Data] {
        guard FileManager.default.fileExists(atPath: support.path) else { return [:] }
        return try Dictionary(uniqueKeysWithValues: FileManager.default.contentsOfDirectory(atPath: support.path).map {
            ($0, try Data(contentsOf: support.appendingPathComponent($0)))
        })
    }
    func model(_ service: MaintenanceConnection = MaintenanceConnection(),
               probe: (@Sendable (ConnectivityProbeMode) async -> ConnectivityReport)? = nil,
               reader: (@Sendable () async throws -> PathObservation)? = nil,
               deletion: (@Sendable () async throws -> Void)? = nil,
               automatic: Bool = false, sleep: (@Sendable (Int) async throws -> Void)? = nil,
               readOnly: Bool = false) -> AppModel {
        AppModel(client: service, isolated: true, readOnly: readOnly, isolatedCredentials: credentials,
            runAutomaticLoopInIsolation: automatic, loopSleep: sleep, isolatedPreferences: preferences,
            recordStoreDirectory: support, isolatedWiFiRecoveryStore: support.appendingPathComponent("wifi-recovery.json"),
            systemConnectivityProbe: probe, networkInformationReader: reader, credentialDeletion: deletion)
    }
}

@main @MainActor
struct AppMaintenanceModelTests {
    private static var checks = 0
    private static var failures: [String] = []
    private static func expect(_ condition: Bool, _ title: String) {
        checks += 1
        if !condition { failures.append(title) }
        print((condition ? "PASS: " : "FAIL: ") + title)
    }
    private static func wait(_ condition: () async -> Bool) async throws {
        let end = ContinuousClock.now.advanced(by: .seconds(5))
        while !(await condition()) {
            precondition(.now < end, "Offline fixture timed out")
            try await Task.sleep(for: .milliseconds(2))
        }
    }
    private static func settle() async {
        for _ in 0..<20 { await Task.yield() }
        try? await Task.sleep(for: .milliseconds(10))
    }
    private static func attach(_ model: AppModel, suffix: String = "a") {
        model.receivePath(usable: true, interfaces: "fixture-wifi-" + suffix,
            identity: NetworkIdentity(fingerprint: "fixture-maintenance-" + suffix),
            descriptors: [NetworkDescriptor(kind: "wifi", raw: ["gw": "a4:5e:60:11:22:33", "gwip": "172.30.0.1", "net": "172.30.0.0/20"])])
    }

    static func main() async throws {
        setbuf(stdout, nil)
        if ProcessInfo.processInfo.arguments.contains("--offline-preview") {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let model = fixture.model()
            let before = try fixture.files()
            expect(model.isPreview && !model.canUninstallApplication, "Offline preview cannot uninstall the formal application")
            expect(await model.prepareForAppMaintenance() == false && !model.isMaintaining,
                "Offline preview refuses the maintenance transition")
            expect(try fixture.files() == before && fixture.preferences.writes == 0,
                "Preview maintenance rejection preserves temporary evidence and all preferences")
            precondition(failures.isEmpty, failures.joined(separator: "\n"))
            print("App maintenance preview: \(checks) assertions passed; no formal dependencies")
            return
        }

        // Three independent workers must truly exit before removal may begin.
        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let connectionGate = MaintenanceGate(), systemGate = MaintenanceGate(), readerGate = MaintenanceGate()
            let service = MaintenanceConnection(probeGate: connectionGate)
            let model = fixture.model(service, probe: { _ in
                await systemGate.enter(); return ConnectivityReport(apple: true, microsoft: true)
            }, reader: {
                await readerGate.enter()
                return PathObservation(usable: true, interfaces: "fixture-late-wifi", constrained: false,
                    continuity: NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: "fixture-late"), linkActive: true))
            })
            attach(model)
            model.checkConnection(); model.refreshDisplayedNetwork()
            try await wait {
                let a = await connectionGate.started, b = await systemGate.started, c = await readerGate.started
                return a == 1 && b == 1 && c == 1
            }
            let barrier = Task { await model.prepareForAppMaintenance() }
            try await wait { model.maintenanceState == .stopping }
            try await wait {
                let a = await connectionGate.cancelled, b = await systemGate.cancelled, c = await readerGate.cancelled
                return a >= 1 && b >= 1 && c >= 1
            }
            expect(!model.canAuthenticate && !model.currentNetworkAuthorized && !model.busy,
                "Maintenance revokes model ownership and authentication before awaiting workers")
            expect(model.maintenanceState == .stopping, "Cancelling suspended fake requests does not prematurely complete the barrier")
            await systemGate.release(); await readerGate.release(); await settle()
            expect(model.maintenanceState == .stopping, "Barrier waits for the final outstanding connection worker")
            await connectionGate.release()
            expect(await barrier.value && model.maintenanceState == .ready,
                "Probe, system probe and metadata reader all finish before maintenance becomes ready")
            let connectionFinished = await connectionGate.finished, systemFinished = await systemGate.finished, readerFinished = await readerGate.finished
            expect(connectionFinished == 1 && systemFinished == 1 && readerFinished == 1,
                "Every captured worker has actually returned")
            expect(model.lastCheckedAt == nil && model.systemConnectivity == nil && model.interfaceSummary != "fixture-late-wifi",
                "Cancelled late success and network observations cannot publish their results")
            expect(!model.canUninstallApplication, "An isolated model never enables a real uninstall dependency")
            let writes = fixture.preferences.writes
            for name in try FileManager.default.contentsOfDirectory(atPath: fixture.support.path) {
                try FileManager.default.removeItem(at: fixture.support.appendingPathComponent(name))
            }
            model.account = "fixture-edited"; model.password = "fixture-edited"; model.area = .dormitory
            model.saveCredentials(); model.revertCredentialDraft(); await model.deleteSavedCredentials()
            model.setMonitoringMode(.recover); model.setProbeMode(.complete); model.setAlwaysShowDockIcon(true)
            model.setLaunchAtLogin(true); model.authorizeCurrentNetwork(); model.resumeAutomaticRecovery()
            model.pauseAutomaticRecovery(); model.cancelCurrentOperation(); model.verifyAndRememberCurrentNetwork()
            model.forgetAllNetworks(); model.forgetCurrentNetwork(); model.forgetNetwork(id: "fixture")
            model.observeNetworkLabel(NetworkObservedLabel(displayName: "Fixture", kind: .wifi, ssid: "Fixture"))
            _ = model.addWebsite("https://fixture.invalid/"); model.setWebsiteStrategy(.direct)
            model.checkConnection(); model.connect(); model.refreshSystemConnectivity(); model.refreshDisplayedNetwork()
            model.setSleeping(true); model.setSleeping(false); model.setForeground(true); attach(model, suffix: "b")
            await model.runAutomaticCycle()
            model.wifiRecovery.setAutoJoinEnabled(true); model.wifiRecovery.setPowerOnWhenDisconnected(true)
            model.wifiRecovery.requestWiFiNamePermission(); model.wifiRecovery.applyPermissionStatus(.authorized)
            await settle()
            expect(try fixture.files().isEmpty && fixture.preferences.writes == writes,
                "Late callbacks and every writable user action leave removed fake data and preferences absent")
            let probes = await service.probes, submissions = await service.submissions
            expect(probes == 1 && submissions == 0 && !model.currentNetworkAuthorized,
                "Maintenance cannot restart checking, authentication or current-network authorization")
            expect(await model.prepareForAppMaintenance(), "A ready maintenance barrier is safely idempotent for retry")
        }

        // A task cancelled earlier is still a live task until its service returns.
        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let gate = MaintenanceGate(), service = MaintenanceConnection(probeGate: gate)
            let model = fixture.model(service)
            attach(model); model.checkConnection(); try await wait { await gate.started == 1 }
            model.cancelCurrentOperation()
            model.checkConnection(); try await wait { await gate.started == 2 }
            let barrier = Task { await model.prepareForAppMaintenance() }
            try await wait { model.maintenanceState == .stopping }
            await gate.release(2); await settle()
            let waitedForRetired = model.maintenanceState == .stopping
            await gate.release(1); _ = await barrier.value
            let finished = await gate.finished
            expect(waitedForRetired && finished == 2,
                "Maintenance also drains an earlier cancelled and replaced manual task")
        }

        // Cancellation of a possibly submitted authentication must not clear its protection.
        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let gate = MaintenanceGate(), service = MaintenanceConnection(loginGate: gate, online: false)
            let model = fixture.model(service)
            model.area = .teaching; model.setMonitoringMode(.recover); attach(model); model.connect()
            try await wait { await gate.started == 1 }
            let barrier = Task { await model.prepareForAppMaintenance() }
            try await wait { model.maintenanceState == .stopping }
            let pendingJournal = try RecoveryJournal.read(fixture.support.appendingPathComponent("recovery.json"), now: Date())
            expect(pendingJournal?.holds.contains { $0.reason == .uncertain } == true && model.cooldownRemaining != nil,
                "Maintenance preserves uncertainty and cooldown for a potentially submitted authentication")
            await gate.release(); expect(await barrier.value, "Submitted fake authentication is drained before ready")
            expect(await service.submissions == 1 && model.lastAttemptOutcome == nil && !model.currentNetworkAuthorized,
                "A late accepted fake login neither changes its result nor restores authorization")
            let journal = try RecoveryJournal.read(fixture.support.appendingPathComponent("recovery.json"), now: Date())
            expect(journal?.holds.contains { $0.reason == .uncertain } == true,
                "Retaining data preserves the pending-submission journal for a later application launch")
        }

        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let gate = MaintenanceGate(), service = MaintenanceConnection()
            let model = fixture.model(service, automatic: true, sleep: { _ in await gate.enter() })
            attach(model); model.setMonitoringMode(.observe)
            try await wait { await gate.started == 1 }
            let barrier = Task { await model.prepareForAppMaintenance() }
            try await wait { let cancelled = await gate.cancelled; return model.maintenanceState == .stopping && cancelled >= 1 }
            expect(model.nextCheckAt == nil, "Cancelling a sleeping automatic loop removes its next scheduled check")
            await gate.release(); let completed = await barrier.value, finished = await gate.finished
            expect(completed && finished == 1,
                "Maintenance waits for the automatic loop's actual completion")
            let probes = await service.probes, submissions = await service.submissions
            expect(probes == 0 && submissions == 0,
                "A cancelled loop cannot run a delayed probe or authentication")
        }

        // Independent Wi-Fi controller: revoke its OS-call token, then await the worker.
        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let gate = MaintenanceGate(), radio = MaintenanceWiFi(gate)
            let url = fixture.support.appendingPathComponent("wifi-fixture.json")
            let wifi = WiFiRecoveryController(preferencesURL: url, service: radio,
                startAutomatically: false, requestsSystemPermission: false)
            let network = WiFiRecoveryNetwork(id: "fixture", ssid: "Fixture Campus", displayName: "Fixture Campus")
            wifi.requestWiFiNamePermission(); wifi.setAutoJoinEnabled(true)
            wifi.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [network])
            let scan = Task { await wifi.refreshOnce() }
            try await wait { await gate.started == 1 }
            let barrier = Task { await wifi.prepareForAppMaintenance() }
            try await wait { let authorized = await radio.authorized, cancelled = await gate.cancelled; return !authorized && cancelled >= 1 }
            let finished = await gate.finished
            expect(wifi.status == .paused && finished == 0,
                "Wi-Fi authorization is revoked before an uncancellable fake scan returns")
            await gate.release(); await barrier.value; await scan.value
            try FileManager.default.removeItem(at: url)
            wifi.setAutoJoinEnabled(false); wifi.setPowerOnWhenDisconnected(true); wifi.setNetworkSelected("fixture", selected: false)
            wifi.applyPermissionStatus(.authorized); wifi.requestWiFiNamePermission()
            wifi.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [network])
            await wifi.refreshOnce(); await settle()
            let started = await gate.started
            expect(!FileManager.default.fileExists(atPath: url.path) && wifi.status == .paused && started == 1,
                "Late permission, preference and scheduling callbacks cannot recreate Wi-Fi data or scans")
        }

        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let gate = MaintenanceGate()
            let model = fixture.model(deletion: { await gate.enter() })
            model.setAlwaysShowDockIcon(true); let before = try fixture.files(), writes = fixture.preferences.writes
            let deletion = Task { await model.deleteSavedCredentials() }
            try await wait { model.credentialsDeletionInProgress }
            expect(await model.prepareForAppMaintenance() == false && !model.isMaintaining,
                "Pending ordinary credential deletion blocks the maintenance transition")
            await gate.release(); await deletion.value
            expect(!model.hasSavedCredentials && model.account.isEmpty && !model.credentialsDeletionFailed,
                "Ordinary saved-account deletion continues to work without entering maintenance")
            expect(try fixture.files().filter { $0.key != "events.json" && $0.key != "recovery.json" }
                   == before.filter { $0.key != "events.json" && $0.key != "recovery.json" }
                   && fixture.preferences.writes == writes && model.alwaysShowDockIcon,
                "Ordinary credential deletion retains settings, Dock preference and noncredential data")
            let readOnly = fixture.model(readOnly: true)
            expect(await readOnly.prepareForAppMaintenance() == false && !readOnly.isMaintaining && !readOnly.canUninstallApplication,
                "Observe-only model refuses maintenance and the formal uninstall capability")
        }

        // Full fake cleanup is only started after the barrier, and failure stays frozen.
        do {
            let fixture = try MaintenanceFixture(); defer { fixture.clean() }
            let model = fixture.model()
            attach(model); model.checkConnection(); try await wait { !model.busy }
            let apps = fixture.root.appendingPathComponent("Applications", isDirectory: true)
            let app = apps.appendingPathComponent("OfflineFixture.app", isDirectory: true)
            try FileManager.default.createDirectory(at: app.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
            let trash = fixture.root.appendingPathComponent("TrashFixture.app")
            var steps: [AppUninstallStep] = [], credentials = ["own": "fake", "other": "keep"]
            var failTrash = true
            let environment = AppUninstallEnvironment(executionAllowed: { true }, applicationDirectories: [apps],
                applicationSupportURL: fixture.support, preferenceDomain: "offline-only",
                inspectApplication: {
                    var info = stat(); guard lstat(app.path, &info) == 0 else { throw POSIXError(.ENOENT) }
                    return AppUninstallCandidate(applicationURL: app, resolvedURL: app,
                        executableURL: app.appendingPathComponent("Contents/MacOS/Fixture"), bundleIdentifier: AppUninstallService.bundleIdentifier,
                        identity: AppUninstallFileIdentity(device: UInt64(info.st_dev), inode: UInt64(info.st_ino)),
                        volumeReadOnly: false, parentWritable: true)
                }, unregisterLoginItem: {
                    expect(model.maintenanceState == .ready, "Uninstall mutation starts only after the model barrier is ready")
                    steps.append(.loginItem)
                }, deleteCredentials: { steps.append(.credentials); credentials.removeValue(forKey: "own") },
                removeSupportData: {
                    steps.append(.supportData)
                    try AppUninstallDataRemoval.removeOwnedDirectory($0, expectedParent: fixture.supportParent)
                }, removePreferences: { _ in steps.append(.preferences); fixture.preferences.values.removeAll() },
                trashApplication: { url, _ in
                    steps.append(.application)
                    if failTrash { throw CocoaError(.fileWriteNoPermission) }
                    try FileManager.default.moveItem(at: url, to: trash); return trash
                })
            let service = AppUninstallService(environment: environment)
            let plan = try await service.prepare()
            expect(await model.prepareForAppMaintenance(removeData: true), "Isolated model can exercise its barrier without enabling real uninstall")
            let failed = await service.execute(plan, removeData: true)
            model.acknowledgeUninstallCleanup(loginItemRemoved: true, credentialsRemoved: true)
            expect(!failed.completed && failed.failedStep == .application && model.isMaintaining,
                "Partial fake uninstall failure reports its actual stage and leaves the model frozen")
            model.setMonitoringMode(.recover); model.saveCredentials(); model.setAlwaysShowDockIcon(true)
            model.checkConnection(); attach(model, suffix: "late"); await model.deleteSavedCredentials()
            await settle()
            expect(!FileManager.default.fileExists(atPath: fixture.support.path) && fixture.preferences.values.isEmpty
                && credentials == ["other": "keep"], "Cleanup removes only fake owned data and cannot be undone by model callbacks")
            failTrash = false
            expect(await model.prepareForAppMaintenance(removeData: false) == false && model.uninstallRemovesData,
                "Retry cannot change a full-cleanup maintenance choice to silently retain remaining data")
            expect(await model.prepareForAppMaintenance(removeData: true), "Retry reuses the completed maintenance barrier with its original choice")
            let retried = await service.execute(try await service.prepare(), removeData: true)
            expect(retried.completed && FileManager.default.fileExists(atPath: trash.path),
                "A partially completed cleanup remains safely retryable with the fake app retained")
        }

        // A reset view can discard its local handle while the shared worker remains alive.
        do {
            let gate = MaintenanceGate()
            let runner = CompatibilityRunner(check: { _, _ in await gate.enter(); return CompatInput() })
            runner.run(proxy: "off", tunnel: "off"); try await wait { await gate.started == 1 }
            runner.reset()
            let barrier = Task { await CompatibilityRunner.stopAllForAppMaintenance() }
            try await wait { await gate.cancelled >= 1 }
            expect(await gate.finished == 0, "Diagnostic reset does not hide a still-running compatibility worker from maintenance")
            await gate.release(); await barrier.value
            runner.run(proxy: "off", tunnel: "off"); await settle()
            let finished = await gate.finished, started = await gate.started
            expect(finished == 1 && started == 1 && !runner.running && runner.input == nil,
                "Diagnostic maintenance drains discarded handles and blocks future work")
        }
        precondition(failures.isEmpty, failures.joined(separator: "\n"))
        print("App maintenance: \(checks) assertions passed; temporary files and fake credentials only")
    }
}
