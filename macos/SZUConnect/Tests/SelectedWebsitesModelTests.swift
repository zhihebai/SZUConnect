import Foundation

private actor OfflineConnectionService: ConnectionService {
    struct Snapshot: Sendable {
        let probeModes: [ConnectivityProbeMode]
        let campusActions: Int
    }

    private var probeModes: [ConnectivityProbeMode] = []
    private var campusActions = 0
    private let microsoftState: ConnectivityEndpointState

    init(microsoftState: ConnectivityEndpointState = .success) { self.microsoftState = microsoftState }

    func probe() async -> ConnectivityReport {
        await probe(mode: .quick)
    }

    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        probeModes.append(mode)
        return ConnectivityReport(
            appleResult: .init(state: .success, elapsed: 0.02),
            microsoftResult: .init(state: mode == .quick ? .notCompleted : microsoftState, elapsed: 0.03),
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
    private let gatedRequest: Int
    private var cancelled: [Bool] = []

    init(gatedRequest: Int = 1) { self.gatedRequest = gatedRequest }

    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        started += 1
        if started == gatedRequest && !released {
            await withCheckedContinuation { continuation = $0 }
        }
        completed += 1
        cancelled.append(Task.isCancelled)
        return WebsiteProbeResult(state: .accessible, title: "网站可访问", detail: "延迟的离线夹具结果。",
            statusCode: 200, elapsed: 0.01, displayURL: input, strategy: strategy)
    }

    func counts() -> (started: Int, completed: Int) { (started, completed) }
    func isWaiting() -> Bool { continuation != nil }
    func cancelledCompletions() -> [Bool] { cancelled }

    func release() {
        released = true
        let pending = continuation
        continuation = nil
        pending?.resume()
    }
}

private actor WebsitePlanWaiter {
    private var calls = 0
    private var cancelled = 0
    private var pending: [Int: CheckedContinuation<Void, Error>] = [:]
    func sleep(_ seconds: Int) async throws {
        calls += 1
        let id = calls
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { pending[id] = $0 }
        } onCancel: { Task { await self.cancel(id) } }
    }
    private func cancel(_ id: Int) {
        guard let continuation = pending.removeValue(forKey: id) else { return }
        cancelled += 1; continuation.resume(throwing: CancellationError())
    }
    func snapshot() -> (calls: Int, cancelled: Int) { (calls, cancelled) }
}

@MainActor private final class WebsitePlanClock {
    var date = Date()
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
            check(!model.canAddWebsitePreset(key: SharedLinks.websitePresets[0].key)
                  && !model.addWebsitePreset(key: SharedLinks.websitePresets[0].key),
                  "Read-only preset APIs reject additions before changing the stored list")
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

            let removed = model.customWebsiteTargets[4]
            let retained = model.customWebsiteTargets.filter { $0.id != removed.id }
            model.removeWebsite(removed.id)
            check(model.customWebsiteTargets == retained && model.customWebsiteTargets.count == 11,
                  "Removing a full-list website preserves all remaining identities and order")
            check(model.addWebsite(removed.url), "A removed website can be added again into the twelfth slot")
            let replaced = model.customWebsiteTargets
            let ids = replaced.map(\.id)
            check(replaced.count == 12 && Set(ids).count == 12
                  && ids.allSatisfy { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty },
                  "Remove and re-add produces twelve nonempty unique identities")
            check(Array(replaced.dropLast()) == retained && replaced.last?.id != removed.id
                  && replaced.last?.url == removed.url && model.websiteConfigurationError == nil,
                  "Re-added URL receives a new identity without replacing retained rows or leaving an error")
            let readded = try WebsitePreferences.read(store)
            check(readded.targets == replaced && readded.strategy == .direct,
                  "Re-added full list remains valid after durable reload")
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
            check(model.lastInternetCheck == nil && model.currentHealth.samples.count == 1
                  && model.currentHealth.samples[0].context?.operation == "manual",
                  "A late result stays anonymous historical evidence without changing the current result")
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
            check(model.lastInternetCheck == nil && model.currentHealth.samples.count == 1 && !model.isOnline,
                  "The prior configuration result is historical only, never the current result")
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
        // Stored disabled rows never change the active selected inputs, including during a real pending request.
        for edit in ["add", "edit", "remove", "reorder"] {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let enabled = WebsiteTarget(id: "kept-enabled", url: "https://kept-enabled.invalid/")
            let firstDisabled = WebsiteTarget(id: "kept-disabled", url: "https://kept-disabled.invalid/", enabled: false)
            let secondDisabled = WebsiteTarget(id: "other-disabled", url: "https://other-disabled.invalid/", enabled: false)
            try WebsitePreferences(targets: [firstDisabled, enabled, secondDisabled]).write(store)
            let checker = GatedWebsiteChecker(gatedRequest: 2)
            let (model, client) = readyModel(checker: checker, websiteStore: store)
            model.setProbeMode(.complete)
            model.checkConnection(); try await finishManual(model)
            let previousReport = model.lastInternetCheck!, previousDate = model.lastCheckedAt
            let previousLatency = model.lastLatencyMilliseconds, revision = model.connectivityResultRevision
            let networkRevision = model.networkRevision, next = model.nextCheckAt, holds = model.holds
            model.checkConnection()
            try await waitUntil("disabled-only pending " + edit) { await checker.isWaiting() }
            var edited: [WebsiteTarget]
            switch edit {
            case "add": edited = model.customWebsiteTargets + [WebsiteTarget(id: "new-disabled", url: "https://new-disabled.invalid/", enabled: false)]
            case "edit": edited = model.customWebsiteTargets.map { $0.id == firstDisabled.id
                ? WebsiteTarget(id: $0.id, url: "https://edited-disabled.invalid/", enabled: false) : $0 }
            case "remove": edited = model.customWebsiteTargets.filter { $0.id != firstDisabled.id }
            default: edited = [secondDisabled, firstDisabled, enabled]
            }
            // Normalize-equivalent selected text also cannot manufacture a changed request input.
            edited = edited.map { $0.enabled ? WebsiteTarget(id: $0.id, url: "HTTPS://KEPT-ENABLED.INVALID:443", enabled: true) : $0 }
            check(model.saveCustomWebsites(edited, preferComplete: true), "A disabled-only \(edit) saves while a selected request is pending")
            check(model.busy && model.lastInternetCheck?.completedAt == previousReport.completedAt
                  && model.lastCheckedAt == previousDate && model.lastLatencyMilliseconds == previousLatency
                  && model.connectivityResultRevision == revision && model.networkRevision == networkRevision,
                  "A disabled-only \(edit) preserves current result, timing, latency and request ownership")
            check(model.nextCheckAt == next && model.holds == holds && !model.websiteConfigurationChanged,
                  "A disabled-only \(edit) preserves the plan, protection and current-result validity")
            check(try WebsitePreferences.read(store).targets == model.customWebsiteTargets,
                  "The disabled-only \(edit) is nevertheless persisted as the fully normalized list")
            await checker.release(); try await finishManual(model)
            check(model.lastInternetCheck?.verdict == .allPassed && model.lastInternetCheck?.websites.map(\.targetID) == [enabled.id],
                  "The original selected request completes normally after disabled-only \(edit)")
            check(await checker.cancelledCompletions().allSatisfy { !$0 }, "Disabled-only \(edit) does not cancel a backend callback")
            check(await client.snapshot().campusActions == 0, "Disabled-only \(edit) starts no campus discovery or login")
        }

        // Actual selected identity/URL/order/strategy edits keep the established invalidation behavior.
        for edit in ["enable", "disable", "url", "identity", "order", "strategy"] {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let first = WebsiteTarget(id: "selected-first", url: "https://selected-first.invalid/")
            let second = WebsiteTarget(id: "selected-second", url: "https://selected-second.invalid/")
            let disabled = WebsiteTarget(id: "selected-disabled", url: "https://selected-disabled.invalid/", enabled: false)
            try WebsitePreferences(targets: [first, second, disabled]).write(store)
            let checker = GatedWebsiteChecker(gatedRequest: 3)
            let (model, _) = readyModel(checker: checker, websiteStore: store)
            model.setProbeMode(.complete); model.checkConnection(); try await finishManual(model)
            model.checkConnection(); try await waitUntil("selected-change pending " + edit) { await checker.isWaiting() }
            var edited = model.customWebsiteTargets
            var strategy = model.customWebsiteStrategy
            switch edit {
            case "enable": edited[2].enabled = true
            case "disable": edited[0].enabled = false
            case "url": edited[0].url = "https://changed-selected.invalid/"
            case "identity": edited[0] = WebsiteTarget(id: "replacement-id", url: first.url)
            case "order": edited.swapAt(0, 1)
            default: strategy = .direct
            }
            check(model.saveCustomWebsites(edited, preferComplete: false, strategy: strategy), "Actual selected \(edit) changes save")
            check(!model.busy && model.lastInternetCheck == nil && model.websiteConfigurationChanged,
                  "Actual selected \(edit) changes cancel and invalidate the old current result")
            await checker.release()
            try await waitUntil("selected-change cancelled callbacks " + edit) { await checker.counts().completed == 4 }
            try await waitUntil("selected-change historical completion " + edit) { model.currentHealth.samples.count == 2 }
            check(await checker.cancelledCompletions().contains(true), "Actual selected \(edit) changes cancel the original backend request")
            check(model.lastInternetCheck == nil && model.websiteConfigurationChanged,
                  "A late old \(edit) result cannot overwrite the newly saved selected configuration")
        }

        // Saving a disabled row must not opt quick mode into complete or restart an existing nonempty plan.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let clock = WebsitePlanClock(), waiter = WebsitePlanWaiter(), service = OfflineConnectionService()
            let selected = WebsiteTarget(id: "planned-selected", url: "https://planned-selected.invalid/")
            let store = folder.appendingPathComponent("websites.json")
            try WebsitePreferences(targets: [selected]).write(store)
            let model = AppModel(client: service, isolated: true, now: { clock.date }, websiteChecker: OfflineWebsiteChecker(),
                websiteStore: store, initialWebsiteTargets: [selected],
                runAutomaticLoopInIsolation: true, loopSleep: { try await waiter.sleep($0) })
            model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: .init(fingerprint: "disabled-plan-fixture"))
            model.setMonitoringMode(.observe)
            try await waitUntil("original monitoring plan") { await waiter.snapshot().calls == 1 }
            let next = model.nextCheckAt
            check(next != nil, "The disabled-edit fixture owns an actual pending monitoring deadline")
            clock.date.addTimeInterval(10)
            let disabled = WebsiteTarget(id: "planned-disabled", url: "https://planned-disabled.invalid/", enabled: false)
            check(model.saveCustomWebsites([selected, disabled], preferComplete: true), "A disabled row saves with preferComplete set")
            check(model.probeMode == .quick && model.nextCheckAt == next,
                  "A disabled-only save neither changes quick mode nor advances an already scheduled deadline")
            let waiting = await waiter.snapshot()
            check(waiting.calls == 1 && waiting.cancelled == 0, "A disabled-only save leaves the actual monitoring waiter alive")
            model.setMonitoringMode(.manual)
        }

        // Partial recovery evidence and sticky holds/cooldown are not reset by disabled editing.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let clock = WebsitePlanClock(), journal = folder.appendingPathComponent("recovery.json")
            try RecoveryJournal(generation: 1, holds: [AuthenticationHold(reason: .security, origin: .global)],
                notBefore: clock.date.addingTimeInterval(45), pendingSubmission: false).write(journal)
            let service = OfflineConnectionService(microsoftState: .timeout)
            let selected = WebsiteTarget(id: "partial-selected", url: "https://partial-selected.invalid/")
            let store = folder.appendingPathComponent("websites.json")
            try WebsitePreferences(targets: [selected]).write(store)
            let model = AppModel(client: service, isolated: true, now: { clock.date }, recoveryStore: journal,
                isolatedCredentials: Credentials(account: "partial-fixture-account", password: "partial-fixture-password"),
                websiteChecker: OfflineWebsiteChecker(), websiteStore: store,
                initialWebsiteTargets: [selected])
            model.setMonitoringMode(.recover); model.setProbeMode(.complete)
            model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: .init(fingerprint: "partial-edit-fixture"))
            await model.runAutomaticCycle()
            check(model.partialRecoveryAwaitingConfirmation, "The partial-edit fixture contains meaningful pending recovery evidence")
            model.pauseAutomaticRecovery()
            let holds = model.holds, cooldown = model.cooldownRemaining, attempted = model.partialRecoveryAttempted
            let previous = model.lastInternetCheck?.completedAt, checkedAt = model.lastCheckedAt
            check(model.saveCustomWebsites([selected, WebsiteTarget(id: "partial-disabled", url: "https://partial-disabled.invalid/", enabled: false)]),
                  "Disabled editing saves while partial recovery and safety protection exist")
            check(model.partialRecoveryAwaitingConfirmation && model.partialRecoveryAttempted == attempted
                  && model.holds == holds && model.cooldownRemaining == cooldown && model.pausedForAutomatic,
                  "Disabled editing preserves partial evidence, user pause, security and cooldown")
            check(model.lastInternetCheck?.completedAt == previous && model.lastCheckedAt == checkedAt,
                  "Disabled editing also preserves the actual partial result and its time")
        }

        // A failed disabled edit never cancels a valid in-flight request or replaces its saved state.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let parent = folder.appendingPathComponent("write-parent")
            try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
            let store = parent.appendingPathComponent("websites.json")
            let selected = WebsiteTarget(id: "write-selected", url: "https://write-selected.invalid/")
            try WebsitePreferences(targets: [selected]).write(store)
            let checker = GatedWebsiteChecker(gatedRequest: 2)
            let (model, _) = readyModel(checker: checker, websiteStore: store)
            model.setProbeMode(.complete); model.checkConnection(); try await finishManual(model)
            model.checkConnection(); try await waitUntil("failed disabled edit pending request") { await checker.isWaiting() }
            let revision = model.connectivityResultRevision, previous = model.lastInternetCheck?.completedAt
            let original = model.customWebsiteTargets
            check(!model.saveCustomWebsites(original + [WebsiteTarget(id: "bad-disabled", url: "ftp://invalid-disabled.invalid/", enabled: false)]),
                  "Invalid disabled rows still fail full-list validation")
            try FileManager.default.removeItem(at: parent); try Data("blocked-parent".utf8).write(to: parent)
            check(!model.saveCustomWebsites(original + [WebsiteTarget(id: "write-disabled", url: "https://write-disabled.invalid/", enabled: false)]),
                  "A disabled-row persistence failure is reported")
            check(model.busy && model.customWebsiteTargets == original && model.connectivityResultRevision == revision
                  && model.lastInternetCheck?.completedAt == previous,
                  "Validation and persistence failures preserve active input, current result and request ownership")
            await checker.release(); try await finishManual(model)
            let cancellations = await checker.cancelledCompletions()
            check(model.lastInternetCheck?.verdict == .allPassed && cancellations.allSatisfy { !$0 },
                  "The original check survives both invalid and unwritable disabled edits")
        }

        // Fixed project presets add only a disabled local choice; the user decides whether to test it.
        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let store = folder.appendingPathComponent("websites.json")
            let (model, _) = readyModel(websiteStore: store)
            model.checkConnection(); try await finishManual(model)
            let previous = model.lastInternetCheck?.completedAt, checkedAt = model.lastCheckedAt
            let revision = model.connectivityResultRevision, mode = model.probeMode, plan = model.nextCheckAt
            check(!model.addWebsitePreset(key: "unknown-project-key") && model.customWebsiteTargets.isEmpty
                  && model.websiteConfigurationError == SharedCopy.t("internet.settingsInvalid"),
                  "An unknown preset key cannot introduce an arbitrary URL")
            for preset in SharedLinks.websitePresets {
                check(model.canAddWebsitePreset(key: preset.key) && !model.hasWebsitePreset(key: preset.key), "A known missing \(preset.key) preset is available")
                check(model.addWebsitePreset(key: preset.key), "A known \(preset.key) preset saves from the shared link source")
                check(model.hasWebsitePreset(key: preset.key) && !model.canAddWebsitePreset(key: preset.key)
                      && model.customWebsiteTargets.last?.url == preset.url && model.customWebsiteTargets.last?.enabled == false,
                      "The saved \(preset.key) preset is disabled and cannot be duplicated")
                check(!model.addWebsitePreset(key: preset.key) && model.websiteConfigurationError == SharedCopy.t("internet.duplicateWebsite"),
                      "A duplicate \(preset.key) preset reports the specific duplicate reason")
            }
            check(model.probeMode == mode && model.nextCheckAt == plan && model.lastInternetCheck?.completedAt == previous
                  && model.lastCheckedAt == checkedAt && model.connectivityResultRevision == revision,
                  "Preset additions preserve probe mode, current result, timing and the next plan")
            let ids = model.customWebsiteTargets.map(\.id)
            check(ids.allSatisfy { !$0.isEmpty } && Set(ids).count == ids.count, "Preset rows receive unique nonempty identifiers")
            let first = model.customWebsiteTargets[0], key = SharedLinks.websitePresets[0].key
            model.removeWebsite(first.id)
            check(model.addWebsitePreset(key: key) && model.customWebsiteTargets.last?.id != first.id,
                  "Removing and adding a preset again creates a new valid row without reviving an old identity")
            let storedPresets = try WebsitePreferences.read(store)
            check(model.selectedWebsiteCount == 0 && storedPresets.targets.allSatisfy { !$0.enabled },
                  "Preset persistence never silently enables a website")
        }

        do {
            let canonicalPreset = SharedLinks.websitePresets[0]
            var parts = URLComponents(string: canonicalPreset.url)!
            parts.scheme = "HTTPS"; parts.host = parts.host!.uppercased(); parts.port = 443
            let target = WebsiteTarget(id: "canonical-preset", url: parts.string!, enabled: true)
            let (model, _) = readyModel(targets: [target])
            check(model.hasWebsitePreset(key: canonicalPreset.key) && !model.canAddWebsitePreset(key: canonicalPreset.key)
                  && !model.addWebsitePreset(key: canonicalPreset.key),
                  "Preset duplicate checks use the same canonical scheme, host, port and path rules as manual rows")
            let disabled = (0..<12).map { WebsiteTarget(id: "full-\($0)", url: "https://full-\($0).invalid/", enabled: false) }
            let (full, _) = readyModel(targets: disabled)
            check(!full.canAddWebsitePreset(key: canonicalPreset.key) && !full.addWebsitePreset(key: canonicalPreset.key)
                  && full.customWebsiteTargets == disabled && full.websiteConfigurationError == WebsiteTarget.ValidationError.tooMany.errorDescription,
                  "The twelfth-row limit applies to disabled presets with a precise full-list reason")
            let (readOnly, _) = readyModel(readOnly: true)
            check(!readOnly.canAddWebsitePreset(key: canonicalPreset.key) && !readOnly.addWebsitePreset(key: canonicalPreset.key)
                  && readOnly.customWebsiteTargets.isEmpty, "Read-only mode rejects preset additions in the model as well as the UI")
        }

        do {
            let folder = try tempFolder(); defer { try? FileManager.default.removeItem(at: folder) }
            let parent = folder.appendingPathComponent("preset-parent")
            try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
            let store = parent.appendingPathComponent("websites.json")
            let enabled = WebsiteTarget(id: "preset-busy-selected", url: "https://preset-busy-selected.invalid/")
            try WebsitePreferences(targets: [enabled]).write(store)
            let checker = GatedWebsiteChecker()
            let (model, _) = readyModel(checker: checker, websiteStore: store)
            model.setProbeMode(.complete); model.checkConnection()
            try await waitUntil("busy preset request") { await checker.isWaiting() }
            let preset = SharedLinks.websitePresets[0]
            check(model.canEditWebsiteTargets && !model.canAddWebsitePreset(key: preset.key)
                  && !model.addWebsitePreset(key: preset.key) && model.websiteConfigurationError == SharedCopy.t("internet.settingsBusy"),
                  "Preset additions are specifically unavailable during an active check even when manual list edits are allowed")
            await checker.release(); try await finishManual(model)
            let original = model.customWebsiteTargets, revision = model.connectivityResultRevision
            let previous = model.lastInternetCheck?.completedAt, mode = model.probeMode
            try FileManager.default.removeItem(at: parent); try Data("blocked-preset-parent".utf8).write(to: parent)
            check(model.canAddWebsitePreset(key: preset.key) && !model.addWebsitePreset(key: preset.key)
                  && model.websiteConfigurationError == SharedCopy.t("internet.settingsWriteFailed"),
                  "A preset save failure reports persistence rather than claiming the preset was added")
            check(model.customWebsiteTargets == original && model.connectivityResultRevision == revision
                  && model.lastInternetCheck?.completedAt == previous && model.probeMode == mode,
                  "A failed preset save preserves selected input, the completed result and mode")
        }
        print("\nSelected website AppModel checks passed: \(assertions)")
    }
}
