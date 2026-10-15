import Foundation

private actor OfflineConnectionService: ConnectionService {
    struct Snapshot: Sendable {
        let probeModes: [ConnectivityProbeMode]
        let campusActions: Int
    }

    private var probeModes: [ConnectivityProbeMode] = []
    private var campusActions = 0

    func probe() async -> ConnectivityReport {
        await probe(mode: .quick)
    }

    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        probeModes.append(mode)
        return ConnectivityReport(
            appleResult: .init(state: .success, elapsed: 0.02),
            microsoftResult: .init(state: mode == .quick ? .notCompleted : .success, elapsed: 0.03),
            mode: mode)
    }

    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        campusActions += 1
        return nil
    }

    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        campusActions += 1
        return "unexpected fixture login"
    }

    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        campusActions += 1
        return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
    }

    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        campusActions += 1
        return LoginAttempt(outcome: .unavailable)
    }

    func snapshot() -> Snapshot { Snapshot(probeModes: probeModes, campusActions: campusActions) }
}

private actor OfflineWebsiteChecker: WebsiteChecking {
    private let state: WebsiteProbeState
    private var inputs: [String] = []

    init(state: WebsiteProbeState = .accessible) { self.state = state }

    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        inputs.append(input)
        return WebsiteProbeResult(state: state,
            title: state == .accessible ? "网站可访问" : "网站响应超时",
            detail: "离线夹具结果。", statusCode: state == .accessible ? 200 : nil,
            elapsed: 0.01, displayURL: input, strategy: strategy,
            recoverySignal: state == .timeout ? .connectionFailure : .none)
    }

    func snapshot() -> [String] { inputs }
}

/// Holds one fake website request after it starts, then returns success even after cancellation.
/// This exercises AppModel's operation ownership and revision checks against late results.
private actor GatedWebsiteChecker: WebsiteChecking {
    private var started = 0
    private var completed = 0
    private var released = false
    private var continuation: CheckedContinuation<Void, Never>?

    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        started += 1
        if !released {
            await withCheckedContinuation { continuation = $0 }
        }
        completed += 1
        return WebsiteProbeResult(state: .accessible, title: "网站可访问", detail: "延迟的离线夹具结果。",
            statusCode: 200, elapsed: 0.01, displayURL: input, strategy: strategy)
    }

    func counts() -> (started: Int, completed: Int) { (started, completed) }

    func release() {
        released = true
        let pending = continuation
        continuation = nil
        pending?.resume()
    }
}

@MainActor
@main
struct SelectedWebsitesModelTests {
    private static var assertions = 0

    private static func check(_ condition: Bool, _ message: String) {
        guard condition else { fatalError("FAIL: \(message)") }
        assertions += 1
        print("PASS: \(message)")
    }

    private static func waitUntil(_ description: String,
                                  condition: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(description)")
    }

    private static func finishManual(_ model: AppModel) async throws {
        try await waitUntil("AppModel manual check completion") { !model.busy }
    }

    private static func tempFolder() throws -> URL {
        let folder = FileManager.default.temporaryDirectory
            .appendingPathComponent("szu-selected-sites-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }

    private static func readyModel(client: OfflineConnectionService = OfflineConnectionService(),
                                   checker: any WebsiteChecking = OfflineWebsiteChecker(),
                                   targets: [WebsiteTarget] = [], websiteStore: URL? = nil, readOnly: Bool = false)
      -> (AppModel, OfflineConnectionService) {
        let model = AppModel(client: client, isolated: true, readOnly: readOnly, websiteChecker: checker,
                             websiteStore: websiteStore, initialWebsiteTargets: targets)
        model.receivePath(usable: true, interfaces: "Wi-Fi en0",
                          identity: NetworkIdentity(fingerprint: "selected-websites-fixture"))
        return (model, client)
    }

    static func main() async throws {
        setbuf(stdout, nil)

        // A read-only observer may inspect stored website choices without changing local state or bytes.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let initial = [WebsiteTarget(id: "read-only", url: "https://read-only.invalid/", enabled: false)]
            try WebsitePreferences(targets: initial, strategy: .direct).write(store)
            let original = try Data(contentsOf: store)
            let (model, _) = readyModel(websiteStore: store, readOnly: true)
            check(!model.addWebsite("https://forbidden-add.invalid/"), "Read-only AppModel rejects adding a website")
            check(!model.saveCustomWebsites([], strategy: .system), "Read-only AppModel rejects replacing the website configuration")
            model.removeWebsite("read-only")
            model.setWebsiteEnabled("read-only", enabled: true)
            model.setWebsiteStrategy(.system)
            check(!model.canEditWebsiteTargets && model.customWebsiteTargets == initial && model.customWebsiteStrategy == .direct,
                  "Read-only website add, remove, selection and strategy APIs preserve the active configuration")
            check(try Data(contentsOf: store) == original, "Read-only website mutations preserve original saved bytes")
        }

        // Base connectivity remains authoritative when a selected website fails.
        do {
            let host = "private-customer-731.invalid"
            let target = WebsiteTarget(id: "private-target", url: "https://\(host)/tokenized-path-731")
            let checker = OfflineWebsiteChecker(state: .timeout)
            let (model, client) = readyModel(checker: checker, targets: [target])
            model.account = "offline-fixture-account"
            model.password = "offline-fixture-password"
            model.setProbeMode(.complete)
            model.checkConnection()
            try await finishManual(model)

            let report = model.lastInternetCheck
            check(report?.verdict == .partial, "Base full pass plus one custom failure produces an overall partial verdict")
            check(report?.base.online == true && model.isOnline,
                  "A custom website failure does not turn passing base connectivity offline")
            check(report?.base.allPassed == true && report?.websitePassedCount == 0 && report?.hasOnlyWebsiteIssues == true,
                  "The report keeps both base endpoints passing and the custom endpoint failing")
            check(model.canOfferPartialLogin, "Website connection failure retains the normal manual recovery action")
            let actions = await client.snapshot()
            check(actions.campusActions == 0,
                  "A passing base report never discovers a campus portal or submits a campus login")
            let calls = await checker.snapshot()
            check(calls == [target.url], "The failed result came only from the injected offline website checker")
        }

        // Full mode succeeds only after both base endpoints and every enabled custom target pass.
        do {
            let targets = [
                WebsiteTarget(id: "first", url: "https://first-selected.invalid/path"),
                WebsiteTarget(id: "second", url: "https://second-selected.invalid")
            ]
            let checker = OfflineWebsiteChecker()
            let (model, _) = readyModel(checker: checker, targets: targets)
            model.setProbeMode(.complete)
            model.checkConnection()
            try await finishManual(model)
            check(model.lastInternetCheck?.verdict == .allPassed,
                  "Full selected-site pass reports allPassed")
            check(model.lastInternetCheck?.selectedCount == 2
                  && model.lastInternetCheck?.includedCount == 2
                  && model.lastInternetCheck?.websitePassedCount == 2,
                  "Full report accounts for every selected website")
        }

        // Quick mode retains selected targets while making no website requests.
        do {
            let target = WebsiteTarget(id: "kept", url: "https://quick-kept.invalid")
            let checker = OfflineWebsiteChecker()
            let (model, _) = readyModel(checker: checker, targets: [target])
            model.checkConnection()
            try await finishManual(model)
            let report = model.lastInternetCheck
            let websiteCalls = await checker.snapshot()
            check(report?.verdict == .basicPassed && report?.base.mode == .quick,
                  "Quick mode returns the base-only verdict")
            check(report?.selectedCount == 1 && report?.includedCount == 0
                  && websiteCalls.isEmpty,
                  "Quick mode skips custom requests while retaining the selected count")
            check(model.customWebsiteTargets == [WebsiteTarget(id: "kept", url: "https://quick-kept.invalid/")],
                  "Quick mode keeps the saved website list")
        }

        // Adding a website persists it and opts the next manual check into complete mode.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let (model, _) = readyModel(websiteStore: store)
            check(model.probeMode == .quick, "A new isolated model starts in quick mode")
            check(model.addWebsite("added-selected.invalid/path"), "A valid website can be added")
            check(model.probeMode == .complete && model.customWebsiteTargets.count == 1
                  && model.customWebsiteTargets[0].enabled,
                  "Adding an enabled website selects complete mode")
            check(FileManager.default.fileExists(atPath: store.path),
                  "Adding a website writes the explicitly supplied temporary preferences file")
        }

        // Expand an existing schema 1 three-row file without replacing identities or losing selections.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let legacy = (1...3).map { WebsiteTarget(id: "legacy-\($0)", url: "https://legacy-\($0).invalid/", enabled: $0 != 2) }
            try WebsitePreferences(targets: legacy, strategy: .direct).write(store)
            let originalBytes = try Data(contentsOf: store)
            let checker = OfflineWebsiteChecker()
            let (model, _) = readyModel(checker: checker, websiteStore: store)
            check(model.customWebsiteTargets == legacy && model.customWebsiteStrategy == .direct,
                  "Legacy three-row preferences retain IDs, order, disabled selection and strategy")
            check(try Data(contentsOf: store) == originalBytes, "Loading legacy websites does not rewrite original bytes")
            for index in 4...12 {
                check(model.addWebsite("https://expanded-\(index).invalid/"), "Expanded website \(index) can be durably added")
            }
            let acceptedBytes = try Data(contentsOf: store)
            check(model.customWebsiteTargets.count == 12 && Array(model.customWebsiteTargets.prefix(3)) == legacy,
                  "Adding through twelve preserves all legacy records")
            check(!model.addWebsite("https://thirteen.invalid/") && (try? Data(contentsOf: store)) == acceptedBytes,
                  "The thirteenth website is rejected without replacing the twelve stored entries")
            model.setWebsiteEnabled("legacy-2", enabled: true)
            check(model.selectedWebsiteCount == 12, "All twelve stored websites may be selected")
            model.checkConnection(); try await finishManual(model)
            check(model.lastInternetCheck?.verdict == .allPassed && model.lastInternetCheck?.includedCount == 12,
                  "Complete AppModel check includes all twelve enabled websites")
            check(model.lastInternetCheck?.websites.map(\.targetID) == model.customWebsiteTargets.map(\.id),
                  "Expanded asynchronous results retain each saved row identity and order")
            let loaded = try WebsitePreferences.read(store)
            check(loaded.targets == model.customWebsiteTargets && loaded.strategy == .direct,
                  "Twelve selected websites round-trip in unchanged schema 1")
            let (reloaded, _) = readyModel(websiteStore: store)
            check(reloaded.customWebsiteTargets == model.customWebsiteTargets, "A fresh AppModel restores all twelve website entries")
        }

        // Disabled entries remain configured but never reach the fake checker.
        do {
            let enabled = WebsiteTarget(id: "enabled", url: "https://enabled-selected.invalid")
            let disabled = WebsiteTarget(id: "disabled", url: "https://disabled-selected.invalid", enabled: false)
            let checker = OfflineWebsiteChecker()
            let (model, _) = readyModel(checker: checker, targets: [enabled, disabled])
            model.setProbeMode(.complete)
            model.checkConnection()
            try await finishManual(model)
            let calls = await checker.snapshot()
            check(calls == ["https://enabled-selected.invalid/"],
                  "Disabled targets are skipped by the AppModel check")
            check(model.lastInternetCheck?.selectedCount == 1
                  && model.lastInternetCheck?.includedCount == 1
                  && model.lastInternetCheck?.verdict == .allPassed,
                  "Disabled targets do not count against the selected-site verdict")
        }

        // Invalid, canonical-duplicate, and over-limit edits cannot change saved settings.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let (model, _) = readyModel(websiteStore: store)
            for host in ["one-selected.invalid", "two-selected.invalid", "three-selected.invalid"] + (4...12).map({ "expanded-limit-\($0).invalid" }) {
                check(model.addWebsite(host), "Initial valid target \(host) saves")
            }
            let savedTargets = model.customWebsiteTargets
            let savedMode = model.probeMode
            let savedStrategy = model.customWebsiteStrategy
            let savedData = try Data(contentsOf: store)

            check(!model.addWebsite("https://user:secret@invalid-selected.invalid/"),
                  "A URL with credentials is rejected")
            let afterInvalidData = try Data(contentsOf: store)
            check(model.customWebsiteTargets == savedTargets && model.probeMode == savedMode
                  && model.customWebsiteStrategy == savedStrategy && afterInvalidData == savedData,
                  "Rejecting an invalid URL leaves active and saved configuration unchanged")

            check(!model.addWebsite("HTTPS://ONE-SELECTED.INVALID:443"),
                  "A canonical duplicate URL is rejected")
            let afterDuplicateData = try Data(contentsOf: store)
            check(model.customWebsiteTargets == savedTargets && afterDuplicateData == savedData,
                  "Rejecting a duplicate leaves the saved website list unchanged")

            check(!model.addWebsite("thirteen-selected.invalid"), "A thirteenth custom website is rejected")
            let afterLimitData = try Data(contentsOf: store)
            check(model.customWebsiteTargets == savedTargets && model.probeMode == savedMode
                  && model.customWebsiteStrategy == savedStrategy && afterLimitData == savedData,
                  "Rejecting a thirteenth target leaves active and saved configuration unchanged")
        }

        // The on-disk format round-trips normalized URLs, strategy, and restrictive permissions.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let first = WebsiteTarget(id: "persist-one", url: " HTTPS://ONE-PERSISTED.INVALID:443 ")
            let second = WebsiteTarget(id: "persist-two", url: "https://two-persisted.invalid/path", enabled: false)
            let (writer, _) = readyModel(websiteStore: store)
            check(writer.saveCustomWebsites([first, second], preferComplete: false, strategy: .direct),
                  "Website preferences save with the direct strategy")

            let permissions = try FileManager.default.attributesOfItem(atPath: store.path)[.posixPermissions] as? NSNumber
            check(permissions?.intValue == 0o600, "Website preferences are written with mode 0600")

            let (reader, _) = readyModel(websiteStore: store)
            check(reader.customWebsiteStrategy == .direct, "Website strategy reloads from websites.json")
            check(reader.customWebsiteTargets == [
                WebsiteTarget(id: "persist-one", url: "https://one-persisted.invalid/"), second
            ], "Normalized website URLs, identities, and enabled flags reload from websites.json")
        }

        // A corrupt source remains intact until an explicit successful replacement saves a backup.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let original = Data("{\"schemaVersion\":99,\"privateMarker\":\"keep-this-file\"}".utf8)
            try original.write(to: store)
            let (model, _) = readyModel(websiteStore: store)
            check(model.customWebsiteTargets.isEmpty && model.websiteConfigurationError != nil,
                  "Corrupt website preferences report a read error without loading targets")
            check(try Data(contentsOf: store) == original,
                  "Loading corrupt preferences preserves the original bytes")
            check(model.addWebsite("recovered-selected.invalid"),
                  "An explicit valid save recovers from corrupt website preferences")
            let names = try FileManager.default.contentsOfDirectory(atPath: folder.path)
            let backups = names.filter { $0.hasPrefix("websites-unreadable-") && $0.hasSuffix(".json") }
            let backupData = try backups.first.map { try Data(contentsOf: folder.appendingPathComponent($0)) }
            check(backups.count == 1 && backupData == original,
                  "Explicit recovery quarantines the untouched corrupt file as a byte-identical backup")
            check(model.websiteConfigurationError == nil,
                  "Successful recovery clears the website configuration error")
        }

        // A failed attempt to recover corrupt settings keeps both the source and its quarantine state.
        do {
            let folder = try tempFolder()
            defer {
                try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: folder.path)
                try? FileManager.default.removeItem(at: folder)
            }
            let store = folder.appendingPathComponent("websites.json")
            let original = Data("{\"schemaVersion\":77,\"privateMarker\":\"retry-this-file\"}".utf8)
            try original.write(to: store)
            let (model, _) = readyModel(websiteStore: store)
            let replacement = WebsiteTarget(id: "retry", url: "https://retry-selected.invalid")

            try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: folder.path)
            check(!model.saveCustomWebsites([replacement], preferComplete: false, strategy: .direct),
                  "A corrupt-preferences recovery fails cleanly when its directory is read-only")
            let afterFailedRecovery = try Data(contentsOf: store)
            check(afterFailedRecovery == original && model.customWebsiteTargets.isEmpty
                  && model.customWebsiteStrategy == .system,
                  "Failed recovery preserves the corrupt original and active settings")
            check(model.websiteConfigurationError != nil,
                  "Failed corrupt-preferences recovery keeps a visible error")

            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: folder.path)
            check(model.saveCustomWebsites([replacement], preferComplete: false, strategy: .direct),
                  "Corrupt-preferences recovery succeeds after storage permissions are restored")
            let names = try FileManager.default.contentsOfDirectory(atPath: folder.path)
            let backups = names.filter { $0.hasPrefix("websites-unreadable-") && $0.hasSuffix(".json") }
            let backupData = try backups.first.map { try Data(contentsOf: folder.appendingPathComponent($0)) }
            let reloaded = try WebsitePreferences.read(store)
            check(backups.count == 1 && backupData == original
                  && reloaded.targets == [WebsiteTarget(id: "retry", url: "https://retry-selected.invalid/")],
                  "The retry still quarantines the original before installing the new configuration")
            check(model.websiteConfigurationError == nil,
                  "Successful retry clears the corrupt-preferences error")
        }

        // A later file-system failure cannot partially install the requested configuration.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let parent = folder.appendingPathComponent("store-parent", isDirectory: true)
            try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
            let store = parent.appendingPathComponent("websites.json")
            let original = WebsiteTarget(id: "stable", url: "https://stable-selected.invalid")
            try WebsitePreferences(targets: [original], strategy: .system).write(store)
            let (model, _) = readyModel(websiteStore: store)
            let oldTargets = model.customWebsiteTargets
            let oldStrategy = model.customWebsiteStrategy
            try FileManager.default.removeItem(at: parent)
            try Data("block directory creation".utf8).write(to: parent)

            let replacement = WebsiteTarget(id: "replacement", url: "https://replacement-selected.invalid")
            check(!model.saveCustomWebsites([replacement], preferComplete: true, strategy: .direct),
                  "A failed preferences write is reported")
            check(model.customWebsiteTargets == oldTargets && model.customWebsiteStrategy == oldStrategy
                  && model.probeMode == .quick,
                  "A failed preferences write keeps the active targets, strategy, and probe mode")
            check(model.websiteConfigurationError != nil,
                  "A failed preferences write exposes a user-readable configuration error")
        }

        // Network identity changes invalidate a request that later returns success.
        do {
            let target = WebsiteTarget(id: "late-network", url: "https://late-network-selected.invalid")
            let checker = GatedWebsiteChecker()
            let (model, _) = readyModel(checker: checker, targets: [target])
            model.setProbeMode(.complete)
            model.checkConnection()
            try await waitUntil("gated network website request") { await checker.counts().started == 1 }
            model.receivePath(usable: true, interfaces: "Wi-Fi en1",
                              identity: NetworkIdentity(fingerprint: "different-network"))
            await checker.release()
            try await waitUntil("late network result return") { await checker.counts().completed == 1 }
            try await Task.sleep(for: .milliseconds(30))
            check(model.lastInternetCheck == nil && model.currentHealth.samples.isEmpty,
                  "A late successful website response after a network change is discarded")
            check(!model.isOnline && model.networkRevision > 1,
                  "A stale report cannot restore online state after the network revision changes")
        }

        // Configuration edits also cancel and invalidate an in-flight website check.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let original = WebsiteTarget(id: "late-config", url: "https://late-config-selected.invalid")
            try WebsitePreferences(targets: [original]).write(store)
            let checker = GatedWebsiteChecker()
            let (model, _) = readyModel(checker: checker, websiteStore: store)
            model.setProbeMode(.complete)
            model.checkConnection()
            try await waitUntil("gated configuration website request") { await checker.counts().started == 1 }
            let replacement = WebsiteTarget(id: "new-config", url: "https://new-config-selected.invalid")
            check(model.saveCustomWebsites([replacement], preferComplete: false),
                  "A website configuration edit succeeds while its owned check is pending")
            await checker.release()
            try await waitUntil("late configuration result return") { await checker.counts().completed == 1 }
            try await Task.sleep(for: .milliseconds(30))
            check(model.customWebsiteTargets == [WebsiteTarget(id: "new-config", url: "https://new-config-selected.invalid/")],
                  "The new website configuration remains authoritative after the old request returns")
            check(model.websiteConfigurationChanged,
                  "A stale report cannot clear the changed-configuration indicator")
            check(model.lastInternetCheck == nil && model.currentHealth.samples.isEmpty && !model.isOnline,
                  "A late result for the prior website configuration is discarded")
        }

        // Persistent events and exports contain only aggregate counts, never website identity.
        do {
            let host = "private-customer-992.invalid"
            let url = "https://\(host)/tokenized-private-path-992"
            let target = WebsiteTarget(id: "private-export", url: url)
            let checker = OfflineWebsiteChecker(state: .timeout)
            let (model, _) = readyModel(checker: checker, targets: [target])
            model.setProbeMode(.complete)
            model.checkConnection()
            try await finishManual(model)

            let sample = model.currentHealth.samples.last
            check(sample?.websiteTotal == 1 && sample?.websitePassed == 0,
                  "Connection history stores only the selected and passed website counts")
            let eventText = model.events.map(\.message).joined(separator: "\n")
            let exportData = model.diagnosticExportData
            let exportText = String(decoding: exportData, as: UTF8.self)
            check(!eventText.contains(host) && !eventText.contains(url)
                  && !eventText.contains("tokenized-private-path-992"),
                  "Connection events contain no custom website name or URL")
            check(!exportText.contains(host) && !exportText.contains(url)
                  && !exportText.contains("tokenized-private-path-992"),
                  "Diagnostic export contains no custom website name or URL")
            let decoded = try JSONSerialization.jsonObject(with: exportData) as? [String: Any]
            let samples = decoded?["samples"] as? [[String: Any]]
            check(samples?.last?["websiteTotal"] as? Int == 1
                  && samples?.last?["websitePassed"] as? Int == 0,
                  "Diagnostic export retains the aggregate website counts")
        }

        // Audit: result retains actual launched mode while changing the next preference.
        do {
            let checker = GatedWebsiteChecker()
            let (model, _) = readyModel(checker: checker, targets: [WebsiteTarget(id: "mode", url: "https://mode-audit.invalid")])
            model.setProbeMode(.complete)
            model.checkConnection()
            try await waitUntil("audit mode request") { await checker.counts().started == 1 }
            model.setProbeMode(.quick)
            await checker.release()
            try await finishManual(model)
            check(model.probeMode == .quick && model.lastProbeMode == .complete, "Changing next mode preserves actual launched mode")
            check(model.lastInternetCheck?.base.mode == .complete && model.presentation.headline == "internet.allPassed.title", "Completed current result uses actual full check rather than next quick choice")
        }
        // Diagnostic export must agree with the visible aggregate result.
        do {
            let (model, _) = readyModel(checker: OfflineWebsiteChecker(state: .timeout), targets: [WebsiteTarget(id: "audit", url: "https://audit.invalid")])
            model.setProbeMode(.complete)
            model.checkConnection()
            try await finishManual(model)
            check(model.lastInternetCheck?.verdict == .partial && model.presentation.headline == "internet.websiteIssues.title",
                  "Website-only issues have a distinct main presentation")
            check(model.presentation.primary == "account" && !model.canOfferPartialLogin,
                  "A connection failure without draft credentials offers account setup")
            check(!model.diagnosticSummary.contains("状态：完整检测全部通过")
                  && model.diagnosticSummary.contains("状态：" + SharedCopy.t("internet.websiteIssues.title")),
                  "Exported status agrees with the website-only result")
        }
        // Audit: website configuration and positive probes cannot clear security/user holds.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let journal = folder.appendingPathComponent("recovery.json")
            try Data("invalid-journal-fixture".utf8).write(to: journal)
            let service = OfflineConnectionService()
            let model = AppModel(client: service, isolated: true, recoveryStore: journal,
                isolatedCredentials: Credentials(account: "audit-fake", password: "audit-fake"),
                websiteChecker: OfflineWebsiteChecker())
            model.setMonitoringMode(.recover)
            model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "audit-holds"))
            check(model.pausedForAutomatic, "Unreadable safety journal creates a security hold")
            check(model.addWebsite("audit-held.invalid"), "Website can be saved independently of authentication hold")
            model.checkConnection(); try await finishManual(model)
            check(model.lastInternetCheck?.verdict == .allPassed && model.pausedForAutomatic, "All successful checks retain the security hold")
            model.pauseAutomaticRecovery()
            model.setProbeMode(.quick)
            model.checkConnection(); try await finishManual(model)
            check(model.holds.contains { $0.reason == .userPaused } && model.holds.contains { $0.reason == .security }, "Probe mode change and successful check preserve explicit pause plus safety hold")
            let count = await service.snapshot()
            check(count.campusActions == 0, "Hold audit issues no campus actions")
        }
        print("\nSelected website AppModel checks passed: \(assertions)")
    }
}
