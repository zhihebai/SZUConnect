import Foundation

private actor JournalService: ConnectionService {
    private var reports: [ConnectivityReport] = []
    private var discovery = CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)
    private var reply = LoginAttempt.success("fixture-server-acknowledgement-must-not-be-persisted")
    private var attempts = 0
    private var pauseAttempt = false
    private var waiting: CheckedContinuation<Void, Never>?

    func configure(reports: [ConnectivityReport] = [], discovery: CampusDiscovery? = nil,
                   reply: LoginAttempt? = nil, pauseAttempt: Bool = false) {
        self.reports = reports
        if let discovery { self.discovery = discovery }
        if let reply { self.reply = reply }
        self.pauseAttempt = pauseAttempt
    }
    func count() -> Int { attempts }
    func isWaiting() -> Bool { waiting != nil }
    func release() {
        guard let continuation = waiting else { fatalError("No fake submission to release") }
        waiting = nil
        continuation.resume()
    }
    func probe() async -> ConnectivityReport {
        reports.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : reports.removeFirst()
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery { discovery }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        precondition(area != .automatic)
        attempts += 1
        let result = reply
        if pauseAttempt {
            pauseAttempt = false
            await withCheckedContinuation { waiting = $0 }
        }
        return result
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        fatalError("Expected staged discovery")
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        fatalError("Expected classified attempt")
    }
}

@MainActor private final class JournalClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ interval: TimeInterval) { date.addTimeInterval(interval) }
}

@main @MainActor
struct RecoveryJournalModelTests {
    private static var passed = 0
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let online = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0),
        microsoftResult: .init(state: .success, elapsed: 0), mode: .complete)
    private static let credentials = Credentials(account: "fixture-journal-account", password: "fixture-journal-password")
    private static let fingerprint = "fixture-private-network-fingerprint"

    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        passed += 1
        print("PASS: \(description)")
    }
    private static func waitUntil(_ description: String, _ predicate: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await predicate() { return }
            try await Task.sleep(for: .milliseconds(1))
        }
        fatalError("Timed out waiting for \(description)")
    }
    private static func model(store: URL, clock: JournalClock, area: CampusArea = .teaching,
                              saved: Credentials? = nil, readOnly: Bool = false) -> (AppModel, JournalService) {
        let service = JournalService()
        let model = AppModel(client: service, isolated: true, readOnly: readOnly,
                             now: { clock.date }, recoveryStore: store, isolatedCredentials: saved ?? credentials)
        model.area = area
        model.setMonitoringMode(.recover)
        model.receivePath(usable: true, interfaces: "offline-fixture",
                          identity: NetworkIdentity(fingerprint: fingerprint), now: clock.date)
        return (model, service)
    }
    private static func finishManual(_ model: AppModel) async throws {
        try await waitUntil("manual completion") { !model.busy }
    }
    private static func store(_ root: URL, _ name: String) throws -> URL {
        let folder = root.appendingPathComponent(name, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
        return folder.appendingPathComponent("recovery.json")
    }
    private static func read(_ url: URL, _ clock: JournalClock) throws -> RecoveryJournal {
        guard let journal = try RecoveryJournal.read(url, now: clock.date) else { fatalError("Missing recovery journal") }
        return journal
    }
    private static func checkPrivate(_ url: URL) throws {
        let data = try Data(contentsOf: url)
        let text = String(decoding: data, as: UTF8.self)
        for value in [credentials.account, credentials.password, fingerprint,
                      "fixture-server-acknowledgement-must-not-be-persisted"] {
            check(!text.contains(value), "Journal excludes private fixture value: \(value)")
        }
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        check((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600,
              "Recovery file is readable and writable only by its owner")
    }

    static func main() async throws {
        setbuf(stdout, nil)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-journal-model-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: root) }

        do {
            let url = try store(root, "user-pause")
            let clock = JournalClock()
            let (first, _) = model(store: url, clock: clock)
            first.authorizeCurrentNetwork()
            first.pauseAutomaticRecovery()
            let (restarted, service) = model(store: url, clock: clock)
            check(restarted.holds.contains { $0.reason == .userPaused }, "Explicit user pause survives model recreation")
            check(restarted.automaticRecoveryPaused && restarted.monitoringMode == .recover && restarted.autoReconnect,
                  "Restored user pause is independent of the recover preference")
            check(!restarted.currentNetworkAuthorized, "Network trust does not cross a process boundary")
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await service.count() == 0, "Restored user pause blocks background credential submission")
            try checkPrivate(url)
        }

        do {
            let url = try store(root, "password-rejection")
            let clock = JournalClock()
            let (first, service) = model(store: url, clock: clock)
            first.password = "fixture-changed-saved-password"
            first.saveCredentials()
            let saved = Credentials(account: credentials.account, password: first.password)
            await service.configure(reply: LoginAttempt(outcome: .rejected, rejection: .password, error: .rejected))
            first.connect(); try await finishManual(first)
            let before = try read(url, clock)
            check(before.generation > 1, "Saving changed credentials increments the durable generation")
            check(before.holds.count == 1 && before.holds.first?.rejection == "password", "Explicit password rejection is persisted")
            let (restarted, replacement) = model(store: url, clock: clock, saved: saved)
            check(restarted.holds == first.holds && restarted.automaticRecoveryPaused,
                  "Saved-account password hold applies after recreation with the same generation")
            check(try read(url, clock).generation == before.generation, "Model recreation keeps the stored credential generation")
            clock.advance(100)
            await replacement.configure(reports: [online, offline, offline])
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await replacement.count() == 0 && restarted.automaticRecoveryPaused,
                  "Restored password hold survives public connectivity and cooldown expiry")
            let text = String(decoding: try Data(contentsOf: url), as: UTF8.self)
            check(!text.contains(saved.password), "Changed saved password is excluded from recovery storage")
        }

        do {
            let url = try store(root, "ephemeral-candidate")
            let clock = JournalClock()
            let (first, service) = model(store: url, clock: clock, area: .automatic)
            await service.configure(reply: LoginAttempt(outcome: .rejected, rejection: .areaMismatch, error: .rejected))
            first.connect(); try await finishManual(first)
            check(first.holds.count == 1 && first.holds.first?.epoch != nil, "Current-network rejection creates an in-session candidate hold")
            check(try read(url, clock).holds.isEmpty, "Current-network candidate hold is omitted from the durable journal")
            let (restarted, _) = model(store: url, clock: clock, area: .automatic)
            check(restarted.holds.isEmpty && !restarted.automaticRecoveryPaused,
                  "A new session does not inherit stale network-specific candidate rejection")
            check(restarted.cooldownRemaining == 90, "Dropping a network candidate hold still preserves the submission deadline")
        }

        do {
            let url = try store(root, "accepted-cooldown")
            let clock = JournalClock()
            let (first, service) = model(store: url, clock: clock)
            first.connect(); try await finishManual(first)
            check(await service.count() == 1 && first.lastAttemptOutcome == .success, "Fixture submission is accepted before simulated restart")
            clock.advance(20)
            let (restarted, replacement) = model(store: url, clock: clock)
            check(restarted.cooldownRemaining == 70, "Recreated model restores the remaining 90-second deadline")
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await replacement.count() == 0, "Restart cannot bypass accepted-submission cooldown")
            restarted.resumeAutomaticRecovery()
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await replacement.count() == 0 && restarted.cooldownRemaining == 70,
                  "Explicit recovery resume also cannot bypass the restored deadline")
            clock.advance(70)
            await replacement.configure(reports: [offline, online])
            await restarted.runAutomaticCycle()
            check(await replacement.count() == 1 && restarted.isOnline, "A fresh round can recover when the restored deadline expires")
            try checkPrivate(url)
        }

        do {
            let url = try store(root, "pending-submission")
            let clock = JournalClock()
            let (first, service) = model(store: url, clock: clock)
            await service.configure(pauseAttempt: true)
            await first.runAutomaticCycle()
            let background = Task { await first.runAutomaticCycle() }
            try await waitUntil("pending fake submission") { await service.isWaiting() }
            let beforeReply = try read(url, clock)
            check(beforeReply.holds.contains { $0.reason == .uncertain }, "Pending uncertain marker reaches disk before authentication await returns")
            check(beforeReply.notBefore == clock.date.addingTimeInterval(90), "Pending marker includes the submission cooldown")
            let (restarted, replacement) = model(store: url, clock: clock)
            check(restarted.automaticRecoveryPaused && restarted.holds.contains { $0.reason == .uncertain },
                  "Restart during an unresolved submission safely restores uncertainty")
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await replacement.count() == 0, "Unresolved prior submission is not automatically duplicated")
            // Invalidate the old owner before releasing it. A terminated process could not commit this result.
            first.cancelCurrentOperation()
            check(first.holds.contains { $0.reason == .uncertain } && restarted.holds.contains { $0.reason == .uncertain },
                  "Cancelled unresolved submission has the same uncertainty before and after restart")
            check(first.cooldownRemaining == restarted.cooldownRemaining,
                  "Cancelled unresolved submission keeps the same cooldown before and after restart")
            await service.release()
            await background.value
            await replacement.configure(reports: [online])
            await restarted.runAutomaticCycle()
            check(restarted.isOnline && !restarted.automaticRecoveryPaused,
                  "Read-only positive connectivity settles restored uncertainty")
            check(try read(url, clock).holds.isEmpty && restarted.cooldownRemaining == 90,
                  "Settling uncertainty persists its release while keeping the cooldown")
        }

        do {
            let url = try store(root, "damaged-journal")
            let damaged = Data("{ damaged original recovery record".utf8)
            try damaged.write(to: url)
            let clock = JournalClock()
            let (restarted, service) = model(store: url, clock: clock)
            check(restarted.automaticRecoveryPaused && restarted.holds.contains { $0.reason == .security },
                  "Damaged journal enters a safe security pause")
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await service.count() == 0, "Damaged journal cannot cause automatic authentication")
            check(try Data(contentsOf: url) == damaged, "Loading and checking preserve the damaged file byte for byte")
            let before = try FileManager.default.contentsOfDirectory(at: url.deletingLastPathComponent(), includingPropertiesForKeys: nil)
            check(before.count == 1, "Damaged file is not silently replaced or quarantined before user action")
            restarted.resumeAutomaticRecovery()
            let after = try FileManager.default.contentsOfDirectory(at: url.deletingLastPathComponent(), includingPropertiesForKeys: nil)
            let backups = after.filter { $0.lastPathComponent != url.lastPathComponent }
            check(backups.count == 1, "Explicit recovery preserves exactly one damaged backup")
            check(try Data(contentsOf: backups[0]) == damaged, "Quarantined damaged backup is byte-identical to the original")
            check(try read(url, clock).holds.isEmpty && !restarted.automaticRecoveryPaused,
                  "Explicit recovery writes a usable clean journal and clears the security hold")
            await service.configure(reports: [offline, offline, online])
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(await service.count() == 1 && restarted.isOnline, "Recovery after damaged-file quarantine can authenticate normally")
        }

        do {
            let url = try store(root, "read-only-restore")
            let clock = JournalClock()
            let (first, _) = model(store: url, clock: clock)
            first.pauseAutomaticRecovery()
            let (restarted, service) = model(store: url, clock: clock, readOnly: true)
            restarted.resumeAutomaticRecovery()
            restarted.authorizeCurrentNetwork()
            restarted.connect(); try await finishManual(restarted)
            await restarted.runAutomaticCycle(); await restarted.runAutomaticCycle()
            check(restarted.sessionReadOnly && restarted.automaticRecoveryPaused && !restarted.currentNetworkAuthorized,
                  "Read-only restart cannot resume a saved pause or grant network trust")
            check(await service.count() == 0, "Read-only manual and automatic paths never submit saved credentials")
        }

        do {
            let url = try store(root, "write-failure")
            let clock = JournalClock()
            let (model, service) = model(store: url, clock: clock)
            // Make the already-loaded file path unwritable even when tests run with elevated privileges.
            try FileManager.default.removeItem(at: url)
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false)
            model.connect(); try await finishManual(model)
            check(await service.count() == 0, "Failed pending-marker write prevents even explicit manual submission")
            check(model.automaticRecoveryPaused && model.holds.contains { $0.reason == .security },
                  "Journal write failure safely pauses further authentication")
            check(model.errorMessage != nil, "Journal write failure is visible to the user")
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.count() == 0, "Write failure cannot be bypassed by the background path")
        }

        print("\(passed) recovery-journal AppModel assertions passed; temporary files only, no network or Keychain.")
    }
}
