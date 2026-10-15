import Foundation
import Darwin

/// All transport is scripted in RAM. A gated discovery deliberately returns success after
/// cancellation, so passing tests require the real AppModel to discard obsolete proofs.
private actor MemoryConnectionService: ConnectionService {
    struct Snapshot: Sendable {
        let probes: Int
        let discoveries: [CampusArea]
        let completedDiscoveries: Int
        let logins: Int
    }

    private var report = ConnectivityReport(apple: true, microsoft: true)
    private var discoveries: [CampusArea] = []
    private var replies: [CampusDiscovery] = []
    private var probes = 0
    private var completedDiscoveries = 0
    private var logins = 0
    private var attemptOutcome: LoginOutcome = .unavailable
    private var pauseNextDiscovery = false
    private var continuation: CheckedContinuation<Void, Never>?
    private var bindingAllowed = true

    func configure(report: ConnectivityReport? = nil, replies: [CampusDiscovery] = [], gated: Bool = false,
                   attempt: LoginOutcome = .unavailable, binding: Bool = true) {
        if let report { self.report = report }
        self.replies = replies
        pauseNextDiscovery = gated
        attemptOutcome = attempt
        bindingAllowed = binding
    }

    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) -> Bool { bindingAllowed }

    func probe() async -> ConnectivityReport { probes += 1; return report }

    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(area)
        guard !replies.isEmpty else { fatalError("Unexpected campus discovery: \(area)") }
        let reply = replies.removeFirst()
        if pauseNextDiscovery {
            pauseNextDiscovery = false
            await withCheckedContinuation { continuation = $0 }
        }
        completedDiscoveries += 1
        return reply
    }

    func isWaiting() -> Bool { continuation != nil }

    func release() {
        guard let pending = continuation else { fatalError("No gated discovery to release") }
        continuation = nil
        pending.resume()
    }

    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        fatalError("Remembering must use staged, fresh campus discovery")
    }

    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        logins += 1
        return "Unexpected fixture login"
    }

    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        logins += 1
        if attemptOutcome == .success { report = ConnectivityReport(apple: true, microsoft: true) }
        return LoginAttempt(outcome: attemptOutcome)
    }

    func snapshot() -> Snapshot {
        Snapshot(probes: probes, discoveries: discoveries, completedDiscoveries: completedDiscoveries, logins: logins)
    }
}

@MainActor @main
struct NetworkMemoryAppModelTests {
    private static var assertions = 0
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let both = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let account = "memory-fixture-account-731"
    private static let password = "memory-fixture-password-731"
    private nonisolated static let descriptor = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:73:19:01", "gwip": "172.29.73.1", "net": "172.29.73.0/24",
        "dhcp": "172.29.73.2", "dns": "172.29.73.3", "sfx": "private-memory-731.invalid"
    ])
    private static let otherDescriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:73:19:02", "gwip": "172.28.72.1", "net": "172.28.72.0/24"
    ])

    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        assertions += 1
        print("PASS: \(description)")
    }

    private static func waitUntil(_ description: String, _ condition: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(description)")
    }

    private static func finish(_ model: AppModel) async throws {
        try await waitUntil("memory verification completion") { !model.busy }
    }

    private static func folder() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("szu-memory-model-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private static func fixture(store: URL, service: MemoryConnectionService = MemoryConnectionService(),
                                readOnly: Bool = false, descriptors: [NetworkDescriptor] = [descriptor],
                                recoveryStore: URL? = nil, credentials: Bool = false,
                                labelsStore: URL? = nil) -> AppModel {
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: { date },
            recoveryStore: recoveryStore,
            isolatedCredentials: credentials ? Credentials(account: account, password: password) : nil,
            memoryStore: store, labelsStore: labelsStore)
        attach(model, descriptors: descriptors)
        return model
    }

    private static func attach(_ model: AppModel, identity: String = "private-memory-identity-731",
                               descriptors: [NetworkDescriptor] = [descriptor]) {
        model.receivePath(usable: true, interfaces: "Wi-Fi en0",
                          identity: NetworkIdentity(fingerprint: identity), descriptors: descriptors)
    }

    private static func seed(_ store: URL, descriptor: NetworkDescriptor = descriptor, includeOther: Bool = false) throws -> Data {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        guard memory.remember(descriptor, area: "dormitory", proofTeaching: true, proofDormitory: true,
                              now: date.addingTimeInterval(-7_200), newID: "1234abcd") != nil else {
            fatalError("Invalid seed descriptor")
        }
        if includeOther {
            guard memory.remember(otherDescriptor, area: "teaching", proofTeaching: true, proofDormitory: true,
                                  now: date.addingTimeInterval(-3_600), newID: "5678abcd") != nil else {
                fatalError("Invalid second seed descriptor")
            }
        }
        let data = memory.encoded()
        try NetworkMemoryFile.write(data, to: store)
        return data
    }

    private static func setWritable(_ writable: Bool, directory: URL) throws {
        try FileManager.default.setAttributes([.posixPermissions: writable ? 0o700 : 0o500], ofItemAtPath: directory.path)
    }

    static func main() async throws {
        setbuf(stdout, nil)
        // Permission-denial fixtures depend on an ordinary user, never a privileged test process.
        guard geteuid() != 0 else { fatalError("Run these isolated tests as an ordinary user, not root") }

        // Read-only launches must never revoke or rewrite the user's remembered networks.
        for action in ["current", "all", "single"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let model = fixture(store: store, readOnly: true)
            let originalRecords = model.rememberedNetworks
            switch action {
            case "all": model.forgetAllNetworks()
            case "single": model.forgetNetwork(id: "1234abcd")
            default: model.forgetCurrentNetwork()
            }
            check(try Data(contentsOf: store) == original && model.rememberedNetworks == originalRecords,
                  "Read-only forget \(action) preserves stored bytes and eligibility")
        }

        // A selected current record cancels late proofs; an unrelated record leaves the same operation alive.
        for current in [true, false] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, includeOther: true)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            model.authorizeCurrentNetwork()
            await service.configure(replies: [both], gated: true)
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("single-forget gated proof") { await service.isWaiting() }
            let removedID = current ? "1234abcd" : "5678abcd"
            model.forgetNetwork(id: removedID)
            check(model.busy == !current && model.currentNetworkAuthorized == !current,
                  "Single forget \(current ? "current" : "other") applies cancellation and trust only to its own network")
            check(model.rememberedNetworks.count == 1 && !model.rememberedNetworks.contains(where: { $0.id == removedID }),
                  "Single forget removes exactly the requested record")
            await service.release()
            try await waitUntil("single-forget late proof completion") { await service.snapshot().completedDiscoveries == 1 }
            try await finish(model)
            try await Task.sleep(for: .milliseconds(30))
            check(model.rememberedNetworks.count == 1 && model.currentNetworkAuthorized == !current,
                  "Late current proof cannot restore a revoked record; unrelated revocation does not cancel valid proof")
            check(NetworkMemory.decode(try Data(contentsOf: store), now: date)?.count == 1,
                  "Single revocation remains durable after pending work settles")
            check(await service.snapshot().logins == 0, "Single revocation and late proof never submit credentials")
            let bytes = try Data(contentsOf: store)
            let events = model.events.count
            model.forgetNetwork(id: removedID); model.forgetNetwork(id: "unknown-record")
            check(try Data(contentsOf: store) == bytes && model.events.count == events,
                  "Repeated or unknown per-record revocation is an unchanged no-op")
        }

        // Storage failure suppresses only the selected record while retaining unrelated session trust.
        for current in [true, false] {
            let root = try folder()
            defer { try? setWritable(true, directory: root); try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store, includeOther: true)
            let model = fixture(store: store)
            model.authorizeCurrentNetwork()
            let removedID = current ? "1234abcd" : "5678abcd"
            try setWritable(false, directory: root)
            model.forgetNetwork(id: removedID)
            check(model.memoryWriteFailed && model.rememberedNetworks.count == 1
                  && !model.rememberedNetworks.contains(where: { $0.id == removedID }),
                  "Failed single write suppresses exactly the requested eligibility")
            check(model.currentNetworkAuthorized == !current && (try? Data(contentsOf: store)) == original,
                  "Failed single write preserves disk bytes and unrelated current-network trust")
            attach(model, identity: "forgotten-network-return", descriptors: current ? [descriptor] : [otherDescriptor])
            model.setSleeping(true); model.setSleeping(false)
            check(!model.networkRemembered && !model.currentNetworkAuthorized,
                  "Returning to the singly revoked network cannot restore eligibility after a failed write")
        }

        // An already-online, credential-free user can set up memory using new campus proofs.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            model.checkConnection(); try await finish(model)
            check(model.isOnline && !model.hasCompleteDraft, "Online setup requires no account or password")
            check(model.canVerifyAndRememberNetwork && !model.canRememberNetwork,
                  "A reliable online network can begin verification without cached campus proof")
            await service.configure(replies: [both], gated: true)
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("fresh online proof starts") { await service.isWaiting() }
            check(model.busy && !model.canVerifyAndRememberNetwork, "Verification owns one cancellable manual operation")
            check(model.isVerifyingMemory && model.memoryVerificationMessage == SharedCopy.t("memory.verification.checking"),
                  "Campus verification exposes its own progress without claiming another public probe")
            check(!model.currentNetworkAuthorized && !FileManager.default.fileExists(atPath: store.path),
                  "Pending campus proof grants no trust and creates no memory file")
            model.verifyAndRememberCurrentNetwork()
            check(await service.snapshot().discoveries == [.automatic], "Repeated clicks cannot start duplicate campus discovery")
            await service.release(); try await finish(model)
            let calls = await service.snapshot()
            check(calls.discoveries == [.automatic] && calls.logins == 0 && calls.probes == 1,
                  "Remembering probes both fixed campus services without login or another public probe")
            check(model.networkRemembered && model.rememberedNetworks.count == 1 && model.currentNetworkAuthorized,
                  "Successful proof and storage add one memory record and session trust")
            check(!model.memoryWriteFailed && model.memoryVerificationMessage != nil && model.isOnline,
                  "Successful memory setup reports completion and preserves the online observation")
            check(!model.isVerifyingMemory && model.memoryVerificationMessage == SharedCopy.t("memory.verification.remembered")
                  && model.campusVerificationState == "confirmed",
                  "Completed verification clears progress and reports fresh campus proof separately")
            model.setMonitoringMode(.observe)
            check(model.memoryVerificationMessage == SharedCopy.t("memory.verification.remembered"),
                  "A later mode change does not replace completed feedback with cancellation")
            let attributes = try FileManager.default.attributesOfItem(atPath: store.path)
            check((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600, "Saved memory file has owner-only 0600 permissions")
            let saved = try Data(contentsOf: store)
            let decoded = NetworkMemory.decode(saved, now: date)
            check(decoded?.count == 1 && decoded?.records.first?.proofTeaching == true
                  && decoded?.records.first?.proofDormitory == true,
                  "The saved file decodes with both verified campus proofs")
            let storedText = String(decoding: saved, as: UTF8.self)
            check(descriptor.raw.values.allSatisfy { !storedText.contains($0) }, "Stored memory contains no raw network features")
            let reloaded = fixture(store: store)
            check(reloaded.networkRemembered && reloaded.rememberedNetworks == model.rememberedNetworks,
                  "A new AppModel reloads the saved network eligibility")
            check(!reloaded.currentNetworkAuthorized && reloaded.memoryVerdictNow == .awaitingProof,
                  "Reloading memory grants no session trust and still requires current campus proof")

            let exported = String(decoding: model.diagnosticExportData, as: UTF8.self)
            let summary = model.diagnosticSummary
            let logged = model.events.map(\.message).joined(separator: "\n")
            let forbidden = Array(descriptor.raw.values) + Array(model.rememberedNetworks[0].features.values)
                + [model.rememberedNetworks[0].id, decoded!.salt, "private-memory-identity-731", account, password]
            check((try? JSONSerialization.jsonObject(with: model.diagnosticExportData)) != nil,
                  "The diagnostic export remains valid JSON after remembering a network")
            check(forbidden.allSatisfy { !exported.contains($0) && !summary.contains($0) && !logged.contains($0) },
                  "Diagnostic export, copied summary and events omit raw features, keyed digests, salt, record ID and credentials")
        }

        // Automatic selection may remember a teaching-only campus network after a fresh teaching proof.
        // An explicit dormitory selection still requires both services.
        for (selectedArea, reply, shouldRememberTeaching) in [
            (CampusArea.automatic, CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), true),
            (.automatic, CampusDiscovery(teaching: .unconfirmed, dormitory: .confirmed), false),
            (.dormitory, CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), false)
        ] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            model.area = selectedArea
            await service.configure(replies: [both, reply])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            model.forgetCurrentNetwork()
            let forgotten = try Data(contentsOf: store)
            check(!model.currentNetworkAuthorized && !model.networkRemembered, "Forgetting removes prior successful proof's authority")
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(await service.snapshot().discoveries == [.automatic, .automatic], "Every remember action requests fresh automatic discovery")
            check(!model.currentNetworkAuthorized && model.networkRemembered == shouldRememberTeaching,
                  "Fresh scope proof may remember teaching without granting session trust")
            let saved = try Data(contentsOf: store)
            check((saved != forgotten) == shouldRememberTeaching, "Only the freshly confirmed scope may change memory")
            if shouldRememberTeaching {
                let item = NetworkMemory.decode(saved, now: date)?.records.first
                check(item?.area == "teaching" && item?.proofTeaching == true && item?.proofDormitory == false,
                      "Automatic teaching-only proof is stored without dormitory authorization")
                check(model.dormitoryMemoryLevel == .none, "Teaching-only memory cannot authorize dormitory")
            }
            check(await service.snapshot().logins == 0, "Fresh memory verification sends no credentials")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let labels = root.appendingPathComponent("network-labels.json")
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, labelsStore: labels)
            model.area = .teaching
            model.observeNetworkLabel(.make(kind: .wifi, ssid: "SZU_WLAN"))
            await service.configure(replies: [CampusDiscovery(teaching: .confirmed, dormitory: .notChecked)])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            let item = NetworkMemory.decode(try Data(contentsOf: store), now: date)?.records.first
            check(await service.snapshot().discoveries == [.teaching], "Explicit teaching only checks its HTTPS campus service")
            check(item?.area == "teaching" && item?.proofTeaching == true && item?.proofDormitory == false
                  && !model.currentNetworkAuthorized, "Explicit teaching memory grants no session trust")
            check(model.networkLabels.entries[item!.id]?.ssid == "SZU_WLAN"
                  && model.wifiRecovery.allowedNetworks.first?.ssid == "SZU_WLAN",
                  "A real observed SSID makes the teaching memory eligible for Wi-Fi finding")
            check(await service.snapshot().logins == 0, "Credential-free teaching memory never submits an account")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            model.area = .teaching
            await service.configure(replies: [CampusDiscovery(teaching: .confirmed, dormitory: .notChecked)])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            let item = NetworkMemory.decode(try Data(contentsOf: store), now: date)?.records.first
            check(item?.proofDormitory == false && item?.area == "teaching" && model.dormitoryMemoryLevel == .none,
                  "Teaching refresh downgrades an old dormitory record instead of extending its authority")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)  // verified two hours ago: beyond the touch interval
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.area = .teaching
            await service.configure(report: offline, attempt: .success)
            model.connect(); try await finish(model)
            check(await service.snapshot().logins == 1, "Fixture completes one manual teaching login")
            check(try Data(contentsOf: store) == original,
                  "Teaching login cannot extend the expiry or change the area of an older dormitory proof")
        }

        // Both malformed JSON and a future schema must remain recoverable without implicit rewriting.
        for original in [Data("broken-memory-file-731".utf8),
                         Data(#"{"schema":99,"salt":"0011223344556677","records":[]}"#.utf8)] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            try original.write(to: store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            check(model.memoryUnreadable && !model.canVerifyAndRememberNetwork,
                  "Corrupt or unknown-schema memory disables persistent remembering")
            model.verifyAndRememberCurrentNetwork()
            model.forgetCurrentNetwork(); model.forgetAllNetworks()
            check(try Data(contentsOf: store) == original, "Remember and forget actions preserve an unreadable memory file byte-for-byte")
            check(await service.snapshot().discoveries.isEmpty && model.rememberedNetworks.isEmpty,
                  "Unreadable memory cannot start discovery or restore remembered eligibility")
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized, "Unreadable memory still permits an explicit session-only trust decision")
        }

        // Saving a new network must commit before adding either runtime memory or session trust.
        do {
            let root = try folder()
            defer { try? setWritable(true, directory: root); try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, descriptors: [otherDescriptor])
            let oldRecords = model.rememberedNetworks
            try setWritable(false, directory: root)
            await service.configure(replies: [both])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.memoryWriteFailed && !model.currentNetworkAuthorized && !model.networkRemembered,
                  "A denied memory write grants no new trust or current-network eligibility")
            let preservedData = try Data(contentsOf: store)
            let calls = await service.snapshot()
            check(model.rememberedNetworks == oldRecords && preservedData == original,
                  "A denied write leaves the prior runtime records and saved bytes unchanged")
            check(model.memoryVerificationMessage != nil && calls.logins == 0,
                  "A denied memory write reports failure without submitting credentials")
        }

        // A late proof must not undo explicit cancellation, revocation, path changes or sleep.
        for action in ["cancel", "modeObserve", "forgetCurrent", "forgetAll", "networkChange", "sleep", "descriptorChange"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            await service.configure(replies: [both], gated: true)
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("gated \(action) discovery") { await service.isWaiting() }
            switch action {
            case "cancel": model.cancelCurrentOperation()
            case "modeObserve": model.setMonitoringMode(.observe)
            case "forgetCurrent": model.forgetCurrentNetwork()
            case "forgetAll": model.forgetAllNetworks()
            case "networkChange": attach(model, identity: "changed-memory-identity", descriptors: [otherDescriptor])
            case "sleep": model.setSleeping(true)
            default: attach(model, descriptors: [otherDescriptor])
            }
            if action == "cancel" || action == "modeObserve" {
                check(!model.isVerifyingMemory && model.memoryVerificationMessage == SharedCopy.t("memory.verification.cancelled"),
                      "\(action): cancelled verification has no stale checking feedback")
            }
            await service.release()
            try await waitUntil("late \(action) discovery completion") { await service.snapshot().completedDiscoveries == 1 }
            try await finish(model)
            // The fake returns even though its caller was cancelled. Give its MainActor continuation
            // the same bounded settling window used by the existing selected-websites regression suite.
            try await Task.sleep(for: .milliseconds(30))
            check(!model.currentNetworkAuthorized && model.rememberedNetworks.isEmpty && !model.networkRemembered,
                  "\(action): late successful proof cannot grant trust or restore memory")
            check(!FileManager.default.fileExists(atPath: store.path), "\(action): late proof creates no memory file")
            check(await service.snapshot().logins == 0, "\(action): cancellation path submits no credentials")
        }

        // A successful memory setup is independent of pre-existing automatic-recovery holds.
        for reason in [HoldReason.userPaused, .credentials, .security] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let journalURL = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: reason, area: reason == .credentials ? .dormitory : nil,
                generation: reason == .credentials ? 1 : nil, origin: reason == .userPaused ? .user : .global,
                rejection: reason == .credentials ? "password" : "")
            let journal = RecoveryJournal(generation: 1, holds: [hold], notBefore: date.addingTimeInterval(90), pendingSubmission: false)
            try journal.write(journalURL)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, recoveryStore: journalURL, credentials: true)
            model.area = .dormitory; model.setMonitoringMode(.recover)
            check(model.automaticRecoveryPaused && model.holds == [hold], "\(reason.key): fixture restores the existing hold")
            await service.configure(report: offline, replies: [both])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.networkRemembered && model.currentNetworkAuthorized, "\(reason.key): explicit memory setup may still complete")
            check(model.holds == [hold] && model.automaticRecoveryPaused && model.cooldownRemaining == 90,
                  "\(reason.key): remembering preserves the hold and full submission cooldown")
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().logins == 0 && model.holds == [hold],
                  "\(reason.key): repeated failures after remembering cannot submit credentials through the hold")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let journalURL = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: .credentials, area: .teaching, generation: 1,
                                          origin: .explicitArea, rejection: "password")
            try RecoveryJournal(generation: 1, holds: [hold], notBefore: nil, pendingSubmission: false).write(journalURL)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, recoveryStore: journalURL, credentials: true)
            model.setMonitoringMode(.recover)
            check(model.autoLoginKey == "limited" && !model.pausedForAutomatic,
                  "An automatic-area hold on teaching alone is limited and still has a usable area")
            await service.configure(replies: [CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.networkRemembered && model.holds == [hold] && model.autoLoginKey == "limited",
                  "Teaching memory setup does not clear a partial authentication hold")
            model.resumeAutomaticRecovery()
            check(model.holds.isEmpty, "Only the explicit resume action clears the partial hold")
        }

        // Failed revocation writes suppress eligibility throughout this process even if the old
        // file survives and the same network returns after a different attachment and sleep.
        for forgetAll in [false, true] {
            let root = try folder()
            defer { try? setWritable(true, directory: root); try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.area = .dormitory; model.setMonitoringMode(.recover); model.authorizeCurrentNetwork()
            check(model.networkRemembered && model.currentNetworkAuthorized, "Forget failure fixture starts remembered and trusted")
            try setWritable(false, directory: root)
            if forgetAll { model.forgetAllNetworks() } else { model.forgetCurrentNetwork() }
            check(model.memoryWriteFailed && !model.networkRemembered && model.rememberedNetworks.isEmpty
                  && !model.currentNetworkAuthorized, "Failed \(forgetAll ? "forget-all" : "forget-current") write still suppresses session eligibility")
            check(try Data(contentsOf: store) == original, "Failed revocation preserves the recoverable on-disk bytes")
            attach(model, identity: "temporary-other-identity", descriptors: [otherDescriptor])
            model.setSleeping(true); model.setSleeping(false); attach(model)
            check(!model.networkRemembered && !model.currentNetworkAuthorized,
                  "Network return and wake cannot resurrect a memory suppressed by failed revocation")
            await service.configure(report: offline, replies: [both])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().logins == 0 && !model.currentNetworkAuthorized,
                  "Fresh campus proof cannot auto-authenticate through a memory revoked this session")
        }

        // Unusable or ambiguous inputs do not enter the proof workflow.
        for kind in ["readOnly", "noDescriptor", "unreliable", "multiple", "noIdentity", "unavailable"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let service = MemoryConnectionService()
            let descriptors: [NetworkDescriptor]
            switch kind {
            case "noDescriptor": descriptors = []
            case "unreliable": descriptors = [NetworkDescriptor(kind: "wifi", raw: ["dns": "172.29.73.3"])]
            case "multiple": descriptors = [descriptor, otherDescriptor]
            default: descriptors = [descriptor]
            }
            let model = fixture(store: store, service: service, readOnly: kind == "readOnly", descriptors: descriptors)
            if kind == "noIdentity" { model.receivePath(usable: true, interfaces: "Wi-Fi en0", descriptors: descriptors) }
            if kind == "unavailable" { model.receivePath(usable: false, interfaces: "", descriptors: descriptors) }
            check(!model.canVerifyAndRememberNetwork, "\(kind): unsuitable input disables verification")
            model.verifyAndRememberCurrentNetwork()
            check(await service.snapshot().discoveries.isEmpty && !model.currentNetworkAuthorized
                  && !FileManager.default.fileExists(atPath: store.path),
                  "\(kind): invoking a disabled action neither probes nor writes nor authorizes")
        }

        // Remembered online networks need no user action before an outage occurs.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.setMonitoringMode(.recover)
            model.checkConnection(); try await finish(model)
            check(model.isOnline && model.autoLoginKey == "ready", "Online remembered network does not falsely ask the user to act")
            check(model.memoryVerdictNow == .awaitingProof && !model.currentNetworkAuthorized,
                  "An online readiness label does not grant trust or skip future campus proof")
            check(await service.snapshot().logins == 0, "Online readiness requires no credential request")
            await service.configure(report: offline, replies: [CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            let afterOutage = await service.snapshot()
            check(!model.isOnline && model.autoLoginKey == "awaitingProof" && afterOutage.logins == 0,
                  "After an outage, missing campus proof still blocks remembered-network login")
        }

        // An unavailable physical identity must not instruct the user to click disabled trust controls.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = MemoryConnectionService()
            let model = fixture(store: root.appendingPathComponent("network-memory.json"), service: service, credentials: true)
            model.area = .dormitory
            model.setMonitoringMode(.recover)
            model.receivePath(usable: true, interfaces: "Wi-Fi en0 · 有线 en12", identity: nil,
                              linkActive: false, tunnel: true, descriptors: [], physicalConnected: true)
            check(model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.identifying",
                  "An unidentified physical path explains identification instead of requesting impossible trust")
            check(model.presentation.banner?.key == "banner.networkUnidentified",
                  "An unidentified path never directs the user to a disabled remember button")
            check(!model.canTrustCurrentNetwork && !model.canVerifyAndRememberNetwork && !model.currentNetworkAuthorized,
                  "Improved unavailable-path presentation grants no trust or memory authority")
            check(await service.snapshot().logins == 0, "Identification-only presentation submits no credentials")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = MemoryConnectionService()
            let model = fixture(store: root.appendingPathComponent("network-memory.json"), service: service)
            await service.configure(replies: [both])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.memoryVerificationMessage != nil, "The completed setup has a success message")
            attach(model, identity: "changed-physical-attachment", descriptors: [otherDescriptor])
            check(model.memoryVerificationMessage == nil, "Changing attachment clears the previous network's setup success message")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = MemoryConnectionService()
            let model = fixture(store: root.appendingPathComponent("network-memory.json"), service: service, credentials: true)
            model.area = .automatic; model.setMonitoringMode(.recover)
            await service.configure(report: offline, replies: [both])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(model.presentation.primary == "trust", "The fresh automatic dormitory path requests explicit trust")
            model.receivePath(usable: true, interfaces: "Wi-Fi en0 · 有线 en12", identity: nil,
                              linkActive: false, tunnel: true, descriptors: [], physicalConnected: true)
            check(model.presentation.primary != "trust" && model.presentation.secondary != "trust"
                  && model.presentation.headline == "banner.networkUnidentified.title",
                  "A stale needsTrust engine state cannot offer an unavailable action after attachment loss")
            model.setMonitoringMode(.manual)
            check(model.presentation.primary != "trust", "Manual mode also removes unavailable trust actions")
        }

        // A fully identified mixed attachment is an explicit session permission, never a physical memory record.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.area = .dormitory; model.setMonitoringMode(.recover)
            func observation(_ fingerprint: String) -> PathObservation {
                var snapshot = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: fingerprint), linkActive: true)
                snapshot.tunnel = true; snapshot.physicalLabels = ["有线 en12"]
                snapshot.systemPhysicalLabel = "有线 en12"
                snapshot.connectedPhysicalLabels = ["Wi-Fi en0", "有线 en12"]
                snapshot.hasUsablePhysicalLink = true; snapshot.sessionOnlyTrust = true
                return PathObservation(usable: true, interfaces: "有线 en12", constrained: false, continuity: snapshot)
            }
            model.applyEnvironmentObservation(observation("mixed-path-A"))
            check(model.sessionOnlyTrust && model.canTrustCurrentNetwork && !model.canVerifyAndRememberNetwork,
                  "Verified system primary under a mixed TUN attachment enables session trust without physical remembering")
            check(model.systemPhysicalLabel == "有线 en12" && model.connectedPhysicalLabels.count == 2 && model.tunnelActive,
                  "System primary, attached physical links and tunnel status remain separate presentation facts")
            check(!model.networkRemembered && model.presentation.banner?.key == "banner.sessionTrustOnly",
                  "Mixed attachment cannot borrow another network's memory and offers the matching session-only action")
            model.area = .automatic
            check(model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.sessionAvailable",
                  "An online mixed attachment presents optional session trust without claiming confirmation is unnecessary")
            model.area = .dormitory
            model.verifyAndRememberCurrentNetwork()
            let afterBlockedRemember = await service.snapshot()
            let storedAfterBlockedRemember = try Data(contentsOf: store)
            check(storedAfterBlockedRemember == original && afterBlockedRemember.discoveries.isEmpty,
                  "Session-only remembering is blocked before probes or disk writes")
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized && model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.trustedCombination",
                  "Explicit session trust accurately names the current connection combination")
            check(await service.snapshot().logins == 0, "Explicit mixed-path trust itself sends zero credentials")
            await service.configure(report: offline, replies: [CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().logins == 0, "Session trust cannot skip missing current dormitory proof")
            model.applyEnvironmentObservation(observation("mixed-path-B"))
            let storedAfterChange = try Data(contentsOf: store)
            check(!model.currentNetworkAuthorized && storedAfterChange == original,
                  "Another attachment's changed fingerprint revokes session trust without changing saved physical memory")
            model.authorizeCurrentNetwork()
            model.receivePath(usable: true, interfaces: "有线 en12", identity: NetworkIdentity(fingerprint: "mixed-path-B"),
                              linkActive: true, tunnel: true, descriptors: [], sessionOnlyTrust: false)
            check(!model.currentNetworkAuthorized, "A changed trust scope revokes permission even if an injected fingerprint is unchanged")
            let briefIdentity = NetworkIdentity(fingerprint: "scope-with-brief-interruption", permitsBriefInterruption: true)
            model.receivePath(usable: true, interfaces: "有线 en12", identity: briefIdentity,
                              linkActive: true, tunnel: true, descriptors: [descriptor], sessionOnlyTrust: false)
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized, "The brief-interruption fixture begins with explicit trust")
            model.receivePath(usable: false, interfaces: "有线 en12", identity: briefIdentity,
                              linkActive: true, tunnel: true, descriptors: [descriptor], sessionOnlyTrust: true)
            check(!model.currentNetworkAuthorized, "A changed trust scope cannot enter the old network's interruption grace")
            model.receivePath(usable: true, interfaces: "有线 en12", identity: briefIdentity,
                              linkActive: true, tunnel: true, descriptors: [descriptor], sessionOnlyTrust: true)
            check(!model.networkRemembered && model.dormitoryMemoryLevel == .none,
                  "Session-only scope ignores physical memory even if a caller supplies a descriptor")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = MemoryConnectionService()
            let model = fixture(store: root.appendingPathComponent("network-memory.json"), service: service, credentials: true)
            model.setProbeMode(.complete)
            model.checkConnection(); try await finish(model)
            let checkedAt = model.lastCheckedAt
            let callsBefore = await service.snapshot()
            model.setProbeMode(.quick)
            let callsAfter = await service.snapshot()
            check(model.probeMode == .quick && model.lastProbeMode == .complete && model.lastInternetCheck?.base.mode == .complete
                  && model.lastCheckedAt == checkedAt,
                  "Selecting quick mode does not relabel an earlier complete report or timestamp")
            check(callsAfter.probes == callsBefore.probes && callsAfter.logins == 0,
                  "Changing probe preference alone starts no connectivity or authentication requests")
            attach(model, identity: "pending-display-new-path", descriptors: [otherDescriptor])
            check(model.lastInternetCheck == nil && model.presentation.headline == "internet.pending.title"
                  && model.presentation.primary == "check",
                  "A current usable attachment without a report displays waiting for detection")
            model.setMonitoringMode(.recover)
            model.pauseAutomaticRecovery()
            check(model.presentation.banner != nil && model.presentation.headline != "internet.pending.title",
                  "Waiting-for-probe display cannot conceal an explicit authentication hold")
        }
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = MemoryConnectionService()
            let model = fixture(store: root.appendingPathComponent("network-memory.json"), service: service)
            await service.configure(binding: false)
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            let calls = await service.snapshot()
            check(!model.busy && !model.isVerifyingMemory && model.memoryVerificationMessage == SharedCopy.t("memory.verification.needsProof"),
                  "Failed transport preparation finishes verification and leaves a visible local result")
            check(calls.discoveries.isEmpty && calls.logins == 0 && !model.networkRemembered,
                  "Rejected binding performs no discovery, memory commit or credential request")
        }
        for selectedArea in CampusArea.allCases {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = MemoryConnectionService()
            let model = fixture(store: root.appendingPathComponent("network-memory.json"), service: service)
            model.area = selectedArea
            await service.configure(replies: [CampusDiscovery(teaching: .unconfirmed, dormitory: .confirmed)])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.campusVerificationState == "unconfirmed" && !model.networkRemembered && !model.currentNetworkAuthorized,
                  "Dormitory HTTP response alone cannot establish verified campus identity or memory in \(selectedArea)")
            let calls = await service.snapshot()
            check(model.memoryVerificationMessage == SharedCopy.t("memory.verification.needsProof")
                  && !model.isVerifyingMemory && calls.logins == 0,
                  "Unconfirmed verification reports its result locally and never submits credentials")
        }
        print("NetworkMemoryAppModelTests: \(assertions) assertions passed")
    }
}
