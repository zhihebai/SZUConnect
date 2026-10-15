import Foundation

/// No persistent domain is queried or modified: every accessor used by AppModel is overridden.
private final class RAMPreferences: UserDefaults, @unchecked Sendable {
    var values: [String: Any]
    var writes = 0
    init(_ values: [String: Any]) {
        self.values = values
        super.init(suiteName: "local.SZUConnect.offline-volatile.\(UUID().uuidString)")!
    }
    override func object(forKey key: String) -> Any? { values[key] }
    override func string(forKey key: String) -> String? { values[key] as? String }
    override func bool(forKey key: String) -> Bool { values[key] as? Bool ?? false }
    override func set(_ value: Any?, forKey key: String) { writes += 1; values[key] = value }
    override func set(_ value: Bool, forKey key: String) { writes += 1; values[key] = value }
}

private actor ReadOnlyService: ConnectionService {
    var probes = 0
    var submissions = 0
    func probe() async -> ConnectivityReport {
        probes += 1
        return ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.001),
                                  microsoftResult: .init(state: .success, elapsed: 0.002), mode: .complete)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { .teaching }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        submissions += 1
        return "fixture-only"
    }
    func counts() -> (Int, Int) { (probes, submissions) }
}

@main @MainActor
struct ReadOnlyAppModelTests {
    private static var checks = 0
    private static func check(_ condition: @autoclosure () throws -> Bool, _ description: String) throws {
        guard try condition() else { fatalError("FAIL: \(description)") }
        checks += 1; print("PASS: \(description)")
    }
    private static func finish(_ model: AppModel) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(3))
        while model.busy, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(1)) }
        try check(!model.busy, "A read-only manual probe finishes normally")
    }
    private static func files(_ directory: URL) throws -> [String: Data] {
        guard FileManager.default.fileExists(atPath: directory.path) else { return [:] }
        return try Dictionary(uniqueKeysWithValues: FileManager.default.contentsOfDirectory(atPath: directory.path).map {
            ($0, try Data(contentsOf: directory.appendingPathComponent($0)))
        })
    }

    static func main() async throws {
        setbuf(stdout, nil)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-readonly-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let date = Date()
        let saved = Credentials(account: "fixture-original-account", password: "fixture-original-password")
        let preferenceValues: [String: Any] = ["area": "teaching", "monitoringMode": "recover", "probeMode": "complete"]

        // Same loading/writing code as production, with only in-memory preferences and temporary files.
        let store = root.appendingPathComponent("existing", isDirectory: true)
        try FileManager.default.createDirectory(at: store, withIntermediateDirectories: false)
        let journal = RecoveryJournal(generation: 1, holds: [], notBefore: date.addingTimeInterval(90), pendingSubmission: true)
        try journal.write(store.appendingPathComponent("recovery.json"))
        try JSONEncoder().encode([ConnectionEvent(date: date, message: "fixture-original-event")])
            .write(to: store.appendingPathComponent("events.json"))
        var health = ConnectionHealth()
        health.append(ConnectionSample(report: ConnectivityReport(apple: true, microsoft: true), interface: "Wi-Fi en0", date: date), now: date)
        try JSONEncoder().encode(health).write(to: store.appendingPathComponent("health.json"))
        try WebsitePreferences(targets: [WebsiteTarget(id: "fixture-site", url: "https://fixture.invalid/", enabled: false)], strategy: .direct)
            .write(store.appendingPathComponent("websites.json"))
        let memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        try NetworkMemoryFile.write(memory.encoded(), to: store.appendingPathComponent("network-memory.json"))
        try NetworkLabels().encoded().write(to: store.appendingPathComponent(NetworkLabelsStore.filename))
        let original = try files(store)
        let preferences = RAMPreferences(preferenceValues)
        let service = ReadOnlyService()
        let model = AppModel(client: service, isolated: true, readOnly: true, now: { date },
                             isolatedCredentials: saved, websiteStore: store.appendingPathComponent("websites.json"),
                             isolatedPreferences: preferences, recordStoreDirectory: store)
        try check(preferences.writes == 0, "Read-only constructor reads preferences without invoking their writer")
        try check(model.area == .teaching && model.monitoringMode == .recover && model.probeMode == .complete,
                  "Read-only construction retains saved choices for detection")
        try check(model.events.count == 1 && model.health.samples.count == 1 && model.holds.contains { $0.reason == .uncertain },
                  "Read-only construction loads existing history and uncertain protection")
        try check(try files(store) == original, "Read-only construction preserves all original file bytes and names")
        let savedHint = model.savedAccountHint
        model.account = "fixture-edited-account"; model.password = "fixture-edited-password"
        model.saveCredentials()
        try check(model.savedAccountHint == savedHint && model.hasUnsavedCredentialChanges,
                  "Read-only save cannot replace the saved credential snapshot")
        model.area = .dormitory; model.area = .teaching
        model.setProbeMode(.quick); model.setProbeMode(.complete)
        model.setMonitoringMode(.manual); model.setMonitoringMode(.observe); model.setMonitoringMode(.recover)
        model.setAutoReconnect(false); model.setAutoReconnect(true)
        model.setLaunchAtLogin(true); model.setLaunchAtLogin(false)
        try check(preferences.writes == 0 && NSDictionary(dictionary: preferences.values).isEqual(to: preferenceValues),
                  "Area, probe and recovery controls change RAM without writing read-only preferences")
        try check(!model.launchAtLogin, "Read-only login-item actions leave the observed login-item state unchanged")
        try check(!model.saveCustomWebsites([]) && !model.addWebsite("https://forbidden.invalid/"),
                  "Read-only website replacements and additions remain blocked")
        model.removeWebsite("fixture-site"); model.setWebsiteEnabled("fixture-site", enabled: true)
        model.setWebsiteStrategy(.system); model.forgetAllNetworks(); model.verifyAndRememberCurrentNetwork()
        model.wifiRecovery.setAutoJoinEnabled(true); model.wifiRecovery.setPowerOnWhenDisconnected(true)
        model.wifiRecovery.requestWiFiNamePermission()
        try check(!model.wifiRecovery.autoJoinEnabled && !model.wifiRecovery.powerOnWhenDisconnected
                  && model.wifiRecovery.permissionStatus == .notRequested,
                  "Read-only Wi-Fi controls cannot alter preferences or request even fake permission")
        model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "fixture-readonly"), now: date)
        model.connect(); try await finish(model)
        model.checkConnection(); try await finish(model)
        await model.runAutomaticCycle()
        model.authorizeCurrentNetwork(); model.resumeAutomaticRecovery(); model.pauseAutomaticRecovery()
        model.cancelCurrentOperation(); model.revertCredentialDraft()
        let counts = await service.counts()
        try check(counts.0 >= 3 && counts.1 == 0 && model.isOnline && !model.canAuthenticate,
                  "Manual and automatic read-only probes update RAM without credential submission")
        try check(model.events.count > 1 && model.health.samples.count > 1,
                  "Read-only detection remains useful with in-memory events and health")
        try check(try files(store) == original, "All read-only user actions and completed probes preserve every saved byte")

        let missing = root.appendingPathComponent("not-created", isDirectory: true)
        let empty = AppModel(client: ReadOnlyService(), isolated: true, readOnly: true, recordStoreDirectory: missing)
        empty.account = "fixture-draft"; empty.password = "fixture-password"; empty.saveCredentials()
        empty.setMonitoringMode(.recover)
        empty.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "fixture-missing"))
        empty.checkConnection(); try await finish(empty)
        try check(!empty.hasSavedCredentials, "Read-only draft save cannot create a saved account")
        try check(!FileManager.default.fileExists(atPath: missing.path), "Read-only load and probes never create a missing record directory")

        let damaged = root.appendingPathComponent("damaged", isDirectory: true)
        try FileManager.default.createDirectory(at: damaged, withIntermediateDirectories: false)
        let corrupted = Data("{damaged-fixture".utf8)
        try corrupted.write(to: damaged.appendingPathComponent("events.json"))
        let damagedOriginal = try files(damaged)
        let corruptModel = AppModel(client: ReadOnlyService(), isolated: true, readOnly: true, recordStoreDirectory: damaged)
        try check(corruptModel.errorMessage != nil && corruptModel.events.isEmpty, "Damaged read-only history is reported without pretending it was loaded")
        corruptModel.area = .teaching; corruptModel.setMonitoringMode(.recover)
        try check(try files(damaged) == damagedOriginal, "Damaged read-only events are never quarantined, renamed or overwritten")

        let writablePreferences = RAMPreferences(preferenceValues)
        let previewModel = AppModel(client: ReadOnlyService(), isolated: true, isolatedPreferences: writablePreferences)
        writablePreferences.writes = 0
        previewModel.account = "fixture-preview-account"; previewModel.password = "fixture-preview-password"
        previewModel.saveCredentials(); previewModel.area = .dormitory; previewModel.setProbeMode(.quick)
        previewModel.setMonitoringMode(.manual)
        previewModel.wifiRecovery.setAutoJoinEnabled(true); previewModel.wifiRecovery.setPowerOnWhenDisconnected(true)
        previewModel.wifiRecovery.requestWiFiNamePermission()
        try check(previewModel.hasSavedCredentials && writablePreferences.writes > 0,
                  "An ordinary isolated preview still supports fake credential and preference actions")
        try check(previewModel.wifiRecovery.autoJoinEnabled && previewModel.wifiRecovery.powerOnWhenDisconnected
                  && previewModel.wifiRecovery.permissionStatus == .authorized,
                  "An ordinary isolated preview still permits simulated Wi-Fi setup")
        print("ReadOnlyAppModelTests: \(checks) passed; RAM preferences, fake service and temporary files only; no Keychain or system changes.")
    }
}
