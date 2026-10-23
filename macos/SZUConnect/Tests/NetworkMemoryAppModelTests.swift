import Foundation
import Darwin

/// All transport is scripted in RAM. A gated discovery deliberately returns success after
/// cancellation, so passing tests require the real AppModel to discard obsolete proofs.
private actor MemoryConnectionService: ConnectionService {
    struct Snapshot: Sendable {
        let probes: Int
        let probeModes: [ConnectivityProbeMode]
        let discoveries: [CampusArea]
        let completedDiscoveries: Int
        let logins: Int
    }

    private var report = ConnectivityReport(apple: true, microsoft: true)
    private var discoveries: [CampusArea] = []
    private var replies: [CampusDiscovery] = []
    private var probes = 0
    private var probeModes: [ConnectivityProbeMode] = []
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

    func probe() async -> ConnectivityReport { await probe(mode: report.mode) }
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        probes += 1; probeModes.append(mode)
        return ConnectivityReport(appleResult: report.appleResult, microsoftResult: report.microsoftResult, mode: mode)
    }

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
        Snapshot(probes: probes, probeModes: probeModes, discoveries: discoveries, completedDiscoveries: completedDiscoveries, logins: logins)
    }
}

private actor VerificationLoopSleep {
    private var delays: [Int] = []
    private let passFirstDelay: Bool
    private let failFirstDelay: Bool
    init(passFirstDelay: Bool = false, failFirstDelay: Bool = false) {
        self.passFirstDelay = passFirstDelay
        self.failFirstDelay = failFirstDelay
    }
    func sleep(_ seconds: Int) async throws {
        delays.append(seconds)
        if delays.count == 1 {
            if failFirstDelay { throw CancellationError() }
            if passFirstDelay { return }
        }
        try await Task.sleep(for: .seconds(3_600))
    }
    func snapshot() -> [Int] { delays }
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
                                labelsStore: URL? = nil, wifiRecoveryStore: URL? = nil) -> AppModel {
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: { date },
            recoveryStore: recoveryStore,
            isolatedCredentials: credentials ? Credentials(account: account, password: password) : nil,
            memoryStore: store, labelsStore: labelsStore, isolatedWiFiRecoveryStore: wifiRecoveryStore)
        attach(model, descriptors: descriptors)
        return model
    }

    private static func attach(_ model: AppModel, identity: String = "private-memory-identity-731",
                               descriptors: [NetworkDescriptor] = [descriptor], usable: Bool = true) {
        model.receivePath(usable: usable, interfaces: "Wi-Fi en0",
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

        // Revocation also covers old copies of the selected identity, including failed writes.
        for writeFails in [false, true] {
            let root = try folder()
            defer { try? setWritable(true, directory: root); try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let seeded = try seed(store, includeOther: true)
            var json = try JSONSerialization.jsonObject(with: seeded) as! [String: Any]
            var records = json["records"] as! [[String: Any]]
            var duplicate = records[0]
            duplicate["id"] = "9abcdeff"
            records.append(duplicate)
            json["records"] = records
            let original = try JSONSerialization.data(withJSONObject: json)
            try NetworkMemoryFile.write(original, to: store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            model.authorizeCurrentNetwork()
            if writeFails { try setWritable(false, directory: root) }
            model.forgetNetwork(id: "9abcdeff")
            check(model.rememberedNetworks.map(\.id) == ["5678abcd"] && !model.currentNetworkAuthorized,
                  "Forgetting a historical duplicate revokes every equivalent record and current authorization")
            check(model.memoryWriteFailed == writeFails, "Equivalent revocation reports the actual storage result")
            if writeFails {
                check(try Data(contentsOf: store) == original, "Failed grouped revocation preserves original stored bytes")
                try setWritable(true, directory: root)
                model.forgetNetwork(id: "5678abcd")
                check(NetworkMemory.decode(try Data(contentsOf: store), now: date)?.count == 0,
                      "A later successful write also removes the earlier session-revoked duplicates")
            } else {
                check(NetworkMemory.decode(try Data(contentsOf: store), now: date)?.records.map(\.id) == ["5678abcd"],
                      "Successful grouped revocation retains the unrelated network")
            }
            check(await service.snapshot().logins == 0, "Grouped revocation never sends credentials")
        }

        do {
            let root = try folder()
            defer { try? setWritable(true, directory: root); try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            try setWritable(false, directory: root)
            model.forgetNetwork(id: "1234abcd")
            check(model.rememberedNetworks.isEmpty, "A failed revocation hides the deactivated record")
            try setWritable(true, directory: root)
            await service.configure(replies: [both])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.rememberedNetworks.map(\.id) == ["1234abcd"] && model.networkRemembered,
                  "Fresh explicit verification restores a previously revoked record only after a successful save")
            check(await service.snapshot().logins == 0, "Restoring network memory still sends no credentials")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, includeOther: true)
            var labels = NetworkLabels()
            _ = labels.updateAutomatic(id: "1234abcd", displayName: "Campus-Fixture", kind: .wifi, ssid: "Campus-Fixture")
            _ = labels.updateAutomatic(id: "5678abcd", displayName: "Campus-Fixture", kind: .wifi, ssid: "Campus-Fixture")
            let labelsURL = root.appendingPathComponent(NetworkLabelsStore.filename)
            check(NetworkLabelsStore(url: labelsURL).save(labels), "Same-name display fixture saves isolated metadata")
            let model = fixture(store: store, labelsStore: labelsURL)
            check(model.rememberedNetworkDisplayName(for: "1234abcd") == "Campus-Fixture"
                  && model.rememberedNetworkDisplayName(for: "5678abcd") == "Campus-Fixture"
                  && model.rememberedNetworkSameNameNoticeIDs == ["1234abcd"],
                  "Same-name identities keep original display names and one notice without merging record-specific controls")
            check(model.currentRememberedNetworkID == "1234abcd"
                  && model.rememberedNetworkDetail(for: model.rememberedNetworks[0]).contains(SharedCopy.t("memory.currentConnection"))
                  && !model.rememberedNetworkDetail(for: model.rememberedNetworks[1]).contains(SharedCopy.t("memory.currentConnection")),
                  "Only the unique actual attachment is marked current even when another network has the same name")
            check(model.rememberedNetworkDetail(for: model.rememberedNetworks[0]).contains(SharedCopy.t("memory.entry.dormitory"))
                  && model.rememberedNetworkDetail(for: model.rememberedNetworks[1]).contains(SharedCopy.t("memory.entry.teaching")),
                  "Remembered network details identify authentication entries rather than physical locations")
            model.forgetNetwork(id: "5678abcd")
            check(model.rememberedNetworks.count == 1 && model.rememberedNetworkDisplayName(for: "1234abcd") == "Campus-Fixture"
                  && model.rememberedNetworkSameNameNoticeIDs.isEmpty && model.currentRememberedNetworkID == "1234abcd",
                  "Forgetting one same-name but different identity keeps the original name and current record intact")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store, includeOther: true)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            check(model.currentRememberedNetworkID == "1234abcd", "A single reliable remembered physical network has one current marker")
            model.refreshDisplayedNetwork()
            check(model.currentRememberedNetworkID == nil, "An environment refresh never labels an in-flight old snapshot current")
            try await waitUntil("display-only refresh") { !model.networkInformationRefreshing }
            check(model.currentRememberedNetworkID == "1234abcd", "The current marker returns after the same snapshot refresh completes")
            model.setSleeping(true)
            check(model.currentRememberedNetworkID == nil, "Sleeping suppresses the current-network marker")
            model.setSleeping(false)
            attach(model, descriptors: [descriptor, otherDescriptor])
            check(model.currentRememberedNetworkID == nil, "Two live descriptors do not choose a current record by best-match order")
            attach(model, descriptors: [otherDescriptor])
            check(model.currentRememberedNetworkID == "5678abcd", "A real attachment change moves the marker to its own record")
            attach(model, usable: false)
            check(model.currentRememberedNetworkID == nil, "Unavailable networks are never marked current")
            model.receivePath(usable: true, interfaces: "Fixture interface", identity: nil, descriptors: [descriptor])
            check(model.currentRememberedNetworkID == nil, "An unidentified attachment cannot borrow a remembered marker")
            attach(model, descriptors: [NetworkDescriptor(kind: "wifi", raw: ["gwip": "172.29.73.1"])])
            check(model.currentRememberedNetworkID == nil, "Weak address-only evidence is insufficient for a current marker")
            attach(model, descriptors: [NetworkDescriptor(kind: "wifi", raw: ["gw": "a4:5e:60:73:19:ff", "gwip": "172.29.73.1", "net": "172.29.73.0/24"])])
            check(model.currentRememberedNetworkID == nil, "Conflicting gateway evidence cannot label a saved network current")
            let identity = NetworkIdentity(fingerprint: "display-only-bound-fixture")
            let route = CampusPhysicalRoute(interfaceName: "en7", interfaceIndex: 7,
                addresses: ["192.0.2.44"], dnsServers: ["192.0.2.53"], identity: "display-fixture", descriptor: descriptor)
            model.receivePath(usable: true, interfaces: "Fixture interface", identity: identity, descriptors: [descriptor],
                connectedPhysicalLabels: ["Wi-Fi en7"], authenticationRoute: route, requiresPhysicalBinding: true)
            check(model.currentRememberedNetworkID == "1234abcd", "A sole bound route may display its uniquely matching remembered record")
            model.receivePath(usable: true, interfaces: "Fixture interface", identity: identity, descriptors: [descriptor],
                connectedPhysicalLabels: ["Wi-Fi en7", "有线 en8"], authenticationRoute: route, requiresPhysicalBinding: true)
            check(model.currentRememberedNetworkID == nil, "A chosen authentication route does not hide a second physical attachment from display ambiguity")
            let wrongRoute = CampusPhysicalRoute(interfaceName: "en8", interfaceIndex: 8,
                addresses: ["192.0.2.45"], dnsServers: ["192.0.2.53"], identity: "display-other", descriptor: otherDescriptor)
            model.receivePath(usable: true, interfaces: "Fixture interface", identity: identity, descriptors: [descriptor],
                authenticationRoute: wrongRoute, requiresPhysicalBinding: true)
            check(model.currentRememberedNetworkID == nil, "A route bound to another descriptor cannot borrow the current display marker")
            model.receivePath(usable: true, interfaces: "Fixture interface", identity: identity, descriptors: [descriptor], sessionOnlyTrust: true)
            check(model.currentRememberedNetworkID == nil, "Session-only identity never appears as a current permanent memory")
            attach(model)
            check(try Data(contentsOf: store) == original, "Display-only marker changes preserve the exact stored network-memory bytes")
            model.forgetNetwork(id: "1234abcd")
            check(model.currentRememberedNetworkID == nil, "Forgetting the current network removes its marker immediately")
            let snapshot = await service.snapshot()
            check(snapshot.probes == 0 && snapshot.discoveries.isEmpty && snapshot.logins == 0,
                  "Reading display markers and refreshing display-only evidence starts no probe, discovery or authentication")
            check(try Data(contentsOf: store) != original, "Only the explicit final forget changes isolated memory bytes")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            var json = try JSONSerialization.jsonObject(with: seed(store)) as! [String: Any]
            var records = json["records"] as! [[String: Any]]
            var duplicate = records[0]; duplicate["id"] = "9abcdeff"; records.append(duplicate)
            json["records"] = records
            let original = try JSONSerialization.data(withJSONObject: json)
            try NetworkMemoryFile.write(original, to: store)
            let model = fixture(store: store)
            check(model.networkRemembered && model.currentRememberedNetworkID == nil,
                  "Duplicate saved matches remain usable by existing recovery policy but have no unique current display marker")
            check(try Data(contentsOf: store) == original, "Display ambiguity never rewrites or merges saved records")
        }

        for failedStore in ["none", "preferences", "memory"] {
            let root = try folder()
            let preferencesFolder = root.appendingPathComponent("wifi-preferences")
            defer {
                try? setWritable(true, directory: root)
                try? setWritable(true, directory: preferencesFolder)
                try? FileManager.default.removeItem(at: root)
            }
            let store = root.appendingPathComponent("network-memory.json")
            var json = try JSONSerialization.jsonObject(with: seed(store)) as! [String: Any]
            var records = json["records"] as! [[String: Any]]
            var duplicate = records[0]; duplicate["id"] = "9abcdeff"; records.append(duplicate)
            json["records"] = records
            let original = try JSONSerialization.data(withJSONObject: json)
            try NetworkMemoryFile.write(original, to: store)
            var labels = NetworkLabels()
            for id in ["1234abcd", "9abcdeff"] {
                _ = labels.updateAutomatic(id: id, displayName: "Campus-Fixture", kind: .wifi, ssid: "Campus-Fixture")
            }
            let labelsURL = root.appendingPathComponent(NetworkLabelsStore.filename)
            check(NetworkLabelsStore(url: labelsURL).save(labels), "Merge preference fixture saves isolated labels")
            let preferencesURL = preferencesFolder.appendingPathComponent("wifi-recovery.json")
            var preferences = WiFiRecoveryPreferences()
            preferences.autoJoinEnabled = true
            preferences.excludedNetworkIDs = ["9abcdeff"]
            try preferences.write(preferencesURL)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, labelsStore: labelsURL, wifiRecoveryStore: preferencesURL)
            check(model.wifiRecovery.selectedNetworkIDs == ["1234abcd"], "Merge starts with only one duplicate selected")
            if failedStore == "preferences" { try setWritable(false, directory: preferencesFolder) }
            if failedStore == "memory" { try setWritable(false, directory: root) }
            await service.configure(replies: [both])
            model.verifyAndRememberCurrentNetwork(); try await finish(model)
            check(model.wifiRecovery.selectedNetworkIDs.isEmpty, "Merging never enables a target excluded through its old record")
            if failedStore == "none" {
                check(model.rememberedNetworks.map(\.id) == ["1234abcd"], "Successful preference commit permits duplicate consolidation")
                let reloaded = fixture(store: store, labelsStore: labelsURL, wifiRecoveryStore: preferencesURL)
                check(reloaded.wifiRecovery.selectedNetworkIDs.isEmpty, "The retained identity remains excluded after restart")
            } else {
                check(try Data(contentsOf: store) == original && model.rememberedNetworks.count == 2,
                      "Failed \(failedStore) persistence leaves every original memory record intact")
                check(model.memoryVerificationMessage == SharedCopy.t("memory.verification.writeFailed"),
                      "Failed \(failedStore) persistence reports that remembering did not complete")
            }
            let savedExclusions = try WiFiRecoveryPreferences.read(preferencesURL).excludedNetworkIDs ?? []
            check(Set(savedExclusions) == (failedStore == "preferences" ? ["9abcdeff"] : ["1234abcd", "9abcdeff"]),
                  "Exclusions persist before any memory replacement, including a later memory write failure")
            check(await service.snapshot().logins == 0, "Preserving merge preferences never submits credentials")
            check(await service.snapshot().discoveries.count == (failedStore == "preferences" ? 0 : 1),
                  "A failed preference preflight prevents even campus discovery")
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
            check(model.campusVerificationInProgress, "An owned in-flight campus operation may display active verification")
            check(!model.currentNetworkAuthorized && !FileManager.default.fileExists(atPath: store.path),
                  "Pending campus proof grants no trust and creates no memory file")
            model.verifyAndRememberCurrentNetwork()
            check(await service.snapshot().discoveries == [.automatic], "Repeated clicks cannot start duplicate campus discovery")
            await service.release(); try await finish(model)
            let calls = await service.snapshot()
            check(calls.discoveries == [.automatic] && calls.logins == 0 && calls.probes == 2,
                  "Remembering obtains one fresh private hint sample and fixed campus proof without logging in")
            check(model.networkRemembered && model.rememberedNetworks.count == 1 && model.currentNetworkAuthorized,
                  "Successful proof and storage add one memory record and session trust")
            check(!model.memoryWriteFailed && model.memoryVerificationMessage != nil && model.isOnline,
                  "Successful memory setup reports completion and preserves the online observation")
            let rememberedFeedback = SharedCopy.t("memory.verification.remembered")
            check(!model.isVerifyingMemory && model.memoryVerificationMessage == rememberedFeedback
                  && model.campusVerificationState == "confirmed",
                  "Completed verification clears progress and reports fresh campus proof separately")
            model.setMonitoringMode(.observe)
            check(model.memoryVerificationMessage == rememberedFeedback,
                  "A later mode change does not replace completed feedback with cancellation")
            model.checkConnection(); try await finish(model)
            check(model.memoryVerificationMessage == rememberedFeedback && !model.campusVerificationInProgress,
                  "An ordinary probe preserves completed feedback and does not invent active campus verification")
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
            let before = NetworkMemory.decode(original, now: date)!
            var expectedRecord = before.records[0]
            expectedRecord.area = "teaching"
            let after = NetworkMemory.decode(try Data(contentsOf: store), now: date)!
            check(after.salt == before.salt && after.records == [expectedRecord],
                  "Teaching login changes only the entry preference, preserving older proof expiry and all other fields")
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
        for action in ["cancel", "modeObserve", "forgetCurrent", "forgetAll", "networkChange", "networkUnavailable", "sleep", "descriptorChange"] {
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
            case "networkUnavailable": attach(model, usable: false)
            case "sleep": model.setSleeping(true)
            default: attach(model, descriptors: [otherDescriptor])
            }
            if action == "cancel" || action == "modeObserve" {
                check(!model.isVerifyingMemory && model.memoryVerificationMessage == SharedCopy.t("memory.verification.cancelled"),
                      "\(action): cancelled verification has no stale checking feedback")
            }
            if action == "networkChange" || action == "descriptorChange" {
                check(model.memoryVerificationMessage == SharedCopy.t("memory.verification.changed"),
                      "A network transition explains why the pending verification ended")
            } else if action == "networkUnavailable" {
                check(model.memoryVerificationMessage == SharedCopy.t("memory.verification.disconnected"),
                      "An unavailable connection ends verification with its own result")
            } else if action == "sleep" {
                check(model.memoryVerificationMessage == SharedCopy.t("memory.verification.sleeping"),
                      "Sleep ends pending verification without leaving a false in-progress message")
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

        // A remembered online network completes its service confirmation as part of
        // the check, without requiring another click or granting session trust.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.setMonitoringMode(.recover)
            await service.configure(replies: [both])
            model.checkConnection(); try await finish(model)
            check(model.isOnline && model.autoLoginKey == "ready", "Online remembered network does not falsely ask the user to act")
            check(model.memoryVerdictNow == .authorized && model.campusVerificationState == "confirmed"
                  && !model.currentNetworkAuthorized && (try? Data(contentsOf: store)) == original,
                  "Automatic service confirmation supplies fresh proof without session trust or memory writes")
            let confirmed = await service.snapshot()
            check(confirmed.discoveries == [.automatic] && confirmed.logins == 0,
                  "Online readiness completes one fixed-service check and requires no credential request")
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
        // A known network can refresh current campus-service evidence without rewriting memory,
        // trusting the session, changing holds, or submitting credentials.
        for (selectedArea, reply, confirmed) in [
            (CampusArea.automatic, both, true),
            (.automatic, CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), true),
            (.teaching, CampusDiscovery(teaching: .confirmed, dormitory: .notChecked), true),
            (.dormitory, CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), false),
            (.automatic, CampusDiscovery(teaching: .unconfirmed, dormitory: .confirmed), false),
            (.automatic, CampusDiscovery(teaching: .unconfirmed, dormitory: .unconfirmed), false)
        ] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let bytes = try seed(store)
            let journalURL = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: .credentials, area: .teaching, generation: 1,
                                          origin: .explicitArea, rejection: "password")
            try RecoveryJournal(generation: 1, holds: [hold], notBefore: date.addingTimeInterval(90), pendingSubmission: false).write(journalURL)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, recoveryStore: journalURL, credentials: true)
            model.area = selectedArea
            model.setMonitoringMode(.recover)
            let records = model.rememberedNetworks
            let savedJournal = try Data(contentsOf: journalURL)
            check(model.networkRemembered && model.canVerifyCurrentCampusService && !model.currentNetworkAuthorized,
                  "Known service-only check is available without session authorization in \(selectedArea)")
            await service.configure(report: offline, replies: [reply], gated: true)
            let visibleReport = model.lastInternetCheck?.base.summary, checkedAt = model.lastCheckedAt
            model.verifyCurrentCampusService()
            try await waitUntil("known network service check") { await service.isWaiting() }
            check(model.busy && model.campusVerificationInProgress && !model.canVerifyCurrentCampusService,
                  "Known service-only check owns one cancellable operation")
            check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.checking"),
                  "Known service-only check explains that it will not log in")
            if model.autoLoginKey == "paused" {
                check(model.readiness.title == "ready.title.paused" && model.readiness.tone == "danger",
                      "Active service-only verification cannot conceal a blocking saved-credential rejection")
            } else if model.autoLoginKey == "limited" {
                check(model.readiness.title == "ready.title.attention" && model.readiness.tone == "warning",
                      "Active service-only verification cannot conceal a scope-limited credential warning")
            }
            model.verifyCurrentCampusService()
            check(await service.snapshot().discoveries.count == 1, "Repeated known service check does not duplicate requests")
            await service.release(); try await finish(model)
            let calls = await service.snapshot()
            check(calls.discoveries == [selectedArea == .teaching ? .teaching : .automatic]
                  && calls.logins == 0 && calls.probeModes == (selectedArea == .automatic ? [.quick] : []),
                  "Known service-only check takes a fresh private quick hint only when automatic selection needs one")
            check(model.lastInternetCheck?.base.summary == visibleReport && model.lastCheckedAt == checkedAt,
                  "Known service-only private hints preserve the visible connectivity observation and timestamp")
            let verificationFeedback = SharedCopy.t(confirmed ? "campus.verify.confirmed" : "campus.verify.failed")
            check(model.campusVerificationState == (confirmed ? "confirmed" : "unconfirmed")
                  && model.memoryVerificationMessage == verificationFeedback,
                  "Known service-only check reports its actual fresh scope proof")
            check(try Data(contentsOf: store) == bytes && model.rememberedNetworks == records,
                  "Known service-only check preserves all remembered IDs, metadata and stored bytes")
            let currentJournal = try Data(contentsOf: journalURL)
            check(!model.currentNetworkAuthorized && model.holds == [hold] && model.cooldownRemaining == 90
                  && currentJournal == savedJournal,
                  "Known service-only check preserves session trust, authentication hold and cooldown")
            check(model.nextCheckAt == nil, "Known service-only completion never creates a new automatic loop")
        }

        // Removing the dual-entry advice is a feedback-only change. Successful
        // discovery still keeps the selected scope and all authentication protections.
        for remember in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let originalBytes = try seed(store)
            let journalURL = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: .credentials, area: .teaching, generation: 1,
                origin: .explicitArea, rejection: "password")
            try RecoveryJournal(generation: 1, holds: [hold], notBefore: date.addingTimeInterval(90), pendingSubmission: false).write(journalURL)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, recoveryStore: journalURL, credentials: true)
            model.setMonitoringMode(.recover)
            let originalJournal = try Data(contentsOf: journalURL)
            let originalRecords = model.rememberedNetworks
            let originalSchedule = model.nextCheckAt
            let visibleReport = model.lastInternetCheck?.base.summary, checkedAt = model.lastCheckedAt
            await service.configure(replies: [both], gated: true)
            if remember { model.verifyAndRememberCurrentNetwork() }
            else { model.verifyCurrentCampusService() }
            try await waitUntil("concise successful campus feedback") { await service.isWaiting() }
            await service.release(); try await finish(model)
            let calls = await service.snapshot()
            check(model.memoryVerificationMessage == SharedCopy.t(remember ? "memory.verification.remembered" : "campus.verify.confirmed")
                  && model.campusVerificationState == "confirmed" && !model.campusVerificationInProgress,
                  "\(remember ? "Remember" : "Service-only") success keeps the confirmation without asking the user to select another entry")
            check(calls.discoveries == [.automatic] && calls.logins == 0 && calls.probeModes == [.quick]
                  && model.lastInternetCheck?.base.summary == visibleReport && model.lastCheckedAt == checkedAt,
                  "\(remember ? "Remember" : "Service-only") uses one private quick sample without login or changing visible detection")
            let savedRecord = model.rememberedNetworks.first
            check(savedRecord?.id == originalRecords.first?.id && savedRecord?.features == originalRecords.first?.features
                  && savedRecord?.area == "dormitory" && savedRecord?.proofTeaching == true && savedRecord?.proofDormitory == true,
                  "\(remember ? "Remember" : "Service-only") concise feedback preserves the original entry and its verified scope")
            check(model.holds == [hold] && model.cooldownRemaining == 90 && model.nextCheckAt == originalSchedule
                  && model.monitoringMode == .recover && model.hasSavedCredentials,
                  "\(remember ? "Remember" : "Service-only") concise feedback keeps saved-account protection and the monitoring schedule")
            check(try Data(contentsOf: journalURL) == originalJournal,
                  "\(remember ? "Remember" : "Service-only") success text cannot rewrite the authentication recovery journal")
            if !remember {
                check(try Data(contentsOf: store) == originalBytes && model.rememberedNetworks == originalRecords,
                      "Service-only concise success preserves original network-memory bytes and proof age")
            }
        }

        // Display-name storage can be unavailable without preventing a credential-free recheck.
        // The service-only path must not perform the metadata preflight used by remember/merge.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let labels = root.appendingPathComponent("network-labels.json")
            let original = try seed(store)
            let damagedLabels = Data("retained-labels-fixture-invalid-json".utf8)
            try damagedLabels.write(to: labels)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, labelsStore: labels)
            check(model.networkLabelsUnavailable && model.canVerifyCurrentCampusService,
                  "Unavailable display metadata does not disable a known service-only check")
            await service.configure(replies: [both])
            model.verifyCurrentCampusService(); try await finish(model)
            let currentLabels = try Data(contentsOf: labels)
            let currentMemory = try Data(contentsOf: store)
            check(model.campusVerificationState == "confirmed" && currentLabels == damagedLabels && currentMemory == original,
                  "Known service-only check preserves damaged label file and original memory bytes")
            check(await service.snapshot().logins == 0, "Known service-only check needs no saved credentials")
        }

        // The same known network is rechecked each time; cached proof cannot satisfy a later click.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            await service.configure(replies: [both, CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)])
            model.verifyCurrentCampusService(); try await finish(model)
            check(model.campusVerificationState == "confirmed", "First known-network click accepts a fresh confirmed proof")
            model.verifyCurrentCampusService(); try await finish(model)
            let calls = await service.snapshot()
            check(calls.discoveries.count == 2 && calls.logins == 0 && model.campusVerificationState == "unconfirmed",
                  "Second known-network click discards its earlier proof and never submits credentials")
            check(try Data(contentsOf: store) == original && !model.currentNetworkAuthorized,
                  "A failed fresh recheck preserves memory without granting trust")
        }

        // Obsolete successful responses cannot survive cancellation, topology, credential,
        // revocation, sleep, or maintenance transitions.
        for action in ["cancel", "area", "account", "modeObserve", "forgetCurrent", "networkChange", "networkUnavailable", "sleep", "descriptorChange", "bindingLost", "deleteCredentials", "maintenance"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            await service.configure(replies: [both], gated: true)
            model.verifyCurrentCampusService()
            try await waitUntil("known service \(action) pending proof") { await service.isWaiting() }
            var maintenance: Task<Bool, Never>?
            switch action {
            case "cancel": model.cancelCurrentOperation()
            case "area": model.area = .teaching
            case "account": model.account = "changed-fixture-account-731"
            case "modeObserve": model.setMonitoringMode(.observe)
            case "forgetCurrent": model.forgetCurrentNetwork()
            case "networkChange": attach(model, identity: "changed-known-identity", descriptors: [otherDescriptor])
            case "networkUnavailable": attach(model, usable: false)
            case "sleep": model.setSleeping(true)
            case "descriptorChange": attach(model, descriptors: [otherDescriptor])
            case "bindingLost":
                model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "private-memory-identity-731"),
                                  descriptors: [descriptor], requiresPhysicalBinding: true)
            case "deleteCredentials": await model.deleteSavedCredentials()
            default:
                maintenance = Task { await model.prepareForAppMaintenance() }
                try await waitUntil("known service maintenance barrier") { model.isMaintaining }
            }
            if ["cancel", "modeObserve"].contains(action) {
                check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.cancelled"),
                      "Known service \(action) reports cancellation without implying data changes")
            } else if ["networkChange", "descriptorChange", "bindingLost"].contains(action) {
                check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.changed"),
                      "Known service \(action) explains the changed attachment")
            } else if action == "networkUnavailable" {
                check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.disconnected"),
                      "Known service interruption explains unavailable connectivity")
            } else if action == "sleep" {
                check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.sleeping"),
                      "Known service sleep explains why verification stopped")
            }
            await service.release()
            try await waitUntil("known service \(action) stale response") { await service.snapshot().completedDiscoveries == 1 }
            if let maintenance { check(await maintenance.value, "Maintenance drains the cancelled service-only task") }
            try await finish(model)
            try await Task.sleep(for: .milliseconds(20))
            check(!model.currentNetworkAuthorized && !model.isVerifyingMemory
                  && model.memoryVerificationMessage != SharedCopy.t("campus.verify.confirmed"),
                  "Known service \(action): late proof cannot grant trust or report success")
            check(await service.snapshot().logins == 0, "Known service \(action) sends zero credentials")
            if action != "forgetCurrent" {
                check(try Data(contentsOf: store) == original, "Known service \(action) preserves remembered bytes")
            } else {
                check(!model.networkRemembered && model.rememberedNetworks.isEmpty,
                      "Known service late proof cannot restore explicitly forgotten memory")
            }
        }

        // Rechecking never bypasses missing physical binding, unrecognized networks, read-only
        // launch, maintenance, or an incompatible descriptor.
        for condition in ["readOnly", "unknown", "bindingMissing", "sessionOnly", "unreliable", "maintenance"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, readOnly: condition == "readOnly", credentials: true)
            switch condition {
            case "unknown": attach(model, identity: "unknown-known-check", descriptors: [otherDescriptor])
            case "bindingMissing":
                model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "private-memory-identity-731"),
                                  descriptors: [descriptor], requiresPhysicalBinding: true)
            case "sessionOnly":
                model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "private-memory-identity-731"),
                                  descriptors: [descriptor], sessionOnlyTrust: true)
            case "unreliable": attach(model, descriptors: [NetworkDescriptor(kind: "wifi", raw: ["net": "172.29.73.0/24"])])
            case "maintenance": _ = await model.prepareForAppMaintenance()
            default: break
            }
            check(!model.canVerifyCurrentCampusService, "Known service check is disabled for \(condition)")
            model.verifyCurrentCampusService(); try await finish(model)
            let calls = await service.snapshot()
            let currentBytes = try Data(contentsOf: store)
            check(calls.discoveries.isEmpty && calls.logins == 0 && currentBytes == original,
                  "Known service \(condition) starts no discovery, authentication, or memory writes")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service)
            await service.configure(binding: false)
            model.verifyCurrentCampusService(); try await finish(model)
            let calls = await service.snapshot()
            check(calls.discoveries.isEmpty && calls.logins == 0
                  && model.memoryVerificationMessage == SharedCopy.t("campus.verify.failed") && !model.currentNetworkAuthorized,
                  "Rejected service-only transport binding stops before discovery and does not trust the session")
        }

        // A service-only action may restore an existing automatic schedule, but not accelerate
        // it into a two-second login round or create one when none previously existed.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let clock = VerificationLoopSleep()
            let service = MemoryConnectionService()
            let model = AppModel(client: service, isolated: true, now: { date },
                isolatedCredentials: Credentials(account: account, password: password), memoryStore: store,
                runAutomaticLoopInIsolation: true, loopSleep: { try await clock.sleep($0) })
            attach(model)
            model.setMonitoringMode(.recover)
            try await waitUntil("original automatic schedule") { await clock.snapshot().count == 1 }
            await service.configure(replies: [both])
            let previousDeadline = model.nextCheckAt!
            let visibleReport = model.lastInternetCheck?.base.summary, checkedAt = model.lastCheckedAt
            model.verifyCurrentCampusService(); try await finish(model)
            try await waitUntil("restored automatic schedule") { await clock.snapshot().count == 2 }
            let delays = await clock.snapshot()
            check(delays[0] == 2 && delays[1] >= 30 && model.nextCheckAt! >= previousDeadline,
                  "Service-only completion restores normal monitoring without advancing the prior deadline")
            let calls = await service.snapshot()
            check(calls.probeModes == [.quick] && calls.logins == 0 && calls.discoveries.count == 1
                  && model.lastInternetCheck?.base.summary == visibleReport && model.lastCheckedAt == checkedAt,
                  "Service-only verification takes one private quick sample without starting an automatic round or authentication")
            model.setMonitoringMode(.manual)
        }

        // Cancel follows the same conservative schedule rule, even when an existing loop
        // had a longer cadence or had already stopped before the explicit check.
        for priorLoopExists in [true, false] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let clock = VerificationLoopSleep(passFirstDelay: priorLoopExists, failFirstDelay: !priorLoopExists)
            let service = MemoryConnectionService()
            if priorLoopExists { await service.configure(replies: [both]) }
            let model = AppModel(client: service, isolated: true, now: { date },
                isolatedCredentials: Credentials(account: account, password: password), memoryStore: store,
                runAutomaticLoopInIsolation: true, loopSleep: { try await clock.sleep($0) })
            model.receivePath(usable: true, interfaces: "Wi-Fi en0", constrained: true,
                identity: NetworkIdentity(fingerprint: "private-memory-identity-731"), descriptors: [descriptor])
            model.setMonitoringMode(.recover)
            if priorLoopExists {
                try await waitUntil("long existing automatic cadence") { await clock.snapshot().count == 2 && model.nextCheckAt == date.addingTimeInterval(120) }
            } else {
                try await waitUntil("stopped automatic cadence") { await clock.snapshot().count == 1 && model.nextCheckAt == nil }
            }
            let callsBefore = await service.snapshot()
            await service.configure(replies: [both], gated: true)
            model.verifyCurrentCampusService()
            try await waitUntil("known service cancellation schedule") { await service.isWaiting() }
            model.cancelCurrentOperation()
            await service.release()
            try await waitUntil("known service cancelled schedule settlement") {
                await service.snapshot().completedDiscoveries == callsBefore.completedDiscoveries + 1
            }
            if priorLoopExists {
                try await waitUntil("restored long cadence") { await clock.snapshot().count == 3 }
                check(await clock.snapshot().last == 120 && model.nextCheckAt == date.addingTimeInterval(120),
                      "Cancelling a service-only check never advances an existing 120-second schedule")
            } else {
                try await Task.sleep(for: .milliseconds(20))
                check(await clock.snapshot().count == 1 && model.nextCheckAt == nil,
                      "Cancelling a service-only check never creates a previously absent automatic loop")
            }
            let callsAfter = await service.snapshot()
            check(callsAfter.logins == 0 && callsAfter.probes == callsBefore.probes + 1
                  && callsAfter.probeModes.last == .quick
                  && model.memoryVerificationMessage == SharedCopy.t("campus.verify.cancelled") && !model.currentNetworkAuthorized,
                  "Service-only cancellation keeps its one private quick sample without resuming a probe or authentication")
            model.setMonitoringMode(.manual)
        }

        // The transient online state becomes genuinely confirmed through the same
        // automatic cycle. Only the separate service result can make readiness green.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.checkConnection(); try await finish(model)
            model.setMonitoringMode(.recover)
            check(model.isOnline && model.networkRemembered && model.campusVerificationState == "notChecked"
                  && model.readiness.title == "ready.title.monitoring" && model.readiness.tone == "info",
                  "Before its next cycle, an online remembered network has no invented campus proof")
            check(!model.currentNetworkAuthorized && model.memoryVerdictNow == .awaitingProof
                  && model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.remembered",
                  "The pending indication preserves unconfirmed campus identity and ungranted session trust")
            await service.configure(replies: [both], gated: true)
            let automaticConfirmation = Task { await model.runAutomaticCycle() }
            try await waitUntil("automatic readiness confirmation") { await service.isWaiting() }
            check(model.readiness.title == "ready.title.checking" && model.campusVerificationInProgress
                  && model.campusVerificationState == "notChecked",
                  "The automatic cycle displays the real pending service confirmation")
            check(!model.currentNetworkAuthorized && (try? Data(contentsOf: store)) == original,
                  "Pending automatic confirmation grants no session trust and preserves memory bytes")
            await service.release(); await automaticConfirmation.value; try await finish(model)
            check(model.campusVerificationState == "confirmed" && model.readiness.title == "ready.title.ready"
                  && model.readiness.tone == "success" && !model.campusVerificationInProgress,
                  "A fresh successful automatic service result completes the visible readiness state")
            let automaticallyConfirmed = await service.snapshot()
            check(automaticallyConfirmed.discoveries == [.automatic] && automaticallyConfirmed.logins == 0
                  && !model.currentNetworkAuthorized && (try? Data(contentsOf: store)) == original,
                  "Completed automatic confirmation sends no credentials and changes no trust or memory")
            await service.configure(replies: [CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)], gated: true)
            model.verifyCurrentCampusService()
            try await waitUntil("readiness real campus verification") { await service.isWaiting() }
            check(model.readiness.title == "ready.title.checking" && model.campusVerificationInProgress,
                  "Only a real in-flight campus-service request displays active verification")
            await service.release(); try await finish(model)
            check(model.campusVerificationState == "unconfirmed" && model.readiness.title != "ready.title.ready"
                  && model.memoryVerificationMessage == SharedCopy.t("campus.verify.failed") && !model.currentNetworkAuthorized,
                  "A failed service-only check keeps visible failure without claiming a confirmed campus")
            check(await service.snapshot().logins == 0 && model.memoryVerdictNow == .awaitingProof,
                  "Readiness wording never grants trust or submits credentials")
            model.pauseAutomaticRecovery()
            check(model.readiness.title == "ready.title.paused" && model.readiness.tone == "warning",
                  "Monitoring wording never overrides an explicit user pause")
        }

        // A teaching-only memory must not borrow dormitory eligibility merely because the
        // network is already online or a later check can reach both services.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            var teaching = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
            _ = teaching.remember(descriptor, area: "teaching", proofTeaching: true, proofDormitory: false,
                now: date.addingTimeInterval(-7_200), newID: "1234abcd")
            let original = teaching.encoded()
            try NetworkMemoryFile.write(original, to: store)
            let service = MemoryConnectionService()
            let model = fixture(store: store, service: service, credentials: true)
            model.area = .dormitory
            model.checkConnection(); try await finish(model)
            model.setMonitoringMode(.recover)
            check(model.isOnline && model.networkRemembered && model.autoLoginKey == "needsTrust"
                  && model.readiness.title == "ready.title.attention",
                  "Teaching-only memory keeps a dormitory trust requirement even while online")
            check(model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.needsTrust"
                  && model.readiness.items.first(where: { $0.kind == "network" })?.tone == "warning",
                  "Monitoring overlay cannot replace a dormitory trust warning with remembered information")
            await service.configure(replies: [both])
            model.verifyCurrentCampusService(); try await finish(model)
            let currentMemory = try Data(contentsOf: store)
            check(model.campusVerificationState == "confirmed" && model.autoLoginKey == "needsTrust"
                  && model.readiness.title == "ready.title.attention" && currentMemory == original
                  && !model.currentNetworkAuthorized && model.dormitoryMemoryLevel == .none,
                  "Service-only proof cannot upgrade teaching-only memory into dormitory trust")
            check(await service.snapshot().logins == 0, "Teaching-only service recheck submits no credentials")
        }

        // New confirmed evidence outranks old service-check feedback. This uses one explicit
        // fake manual login to reach the normal runtime transition; the service-only action
        // itself still sends zero credentials, and no live account or transport is involved.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            var instant = date
            let service = MemoryConnectionService()
            let model = AppModel(client: service, isolated: true, now: { instant },
                isolatedCredentials: Credentials(account: account, password: password), memoryStore: store)
            attach(model)
            model.setMonitoringMode(.recover)
            await service.configure(replies: [CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)])
            model.verifyCurrentCampusService(); try await finish(model)
            let beforeExplicitLogin = await service.snapshot()
            check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.failed")
                  && model.campusVerificationState == "unconfirmed" && beforeExplicitLogin.logins == 0,
                  "A failed service-only fixture retains feedback with zero authentication submissions")
            await service.configure(report: offline, replies: [both], attempt: .success)
            model.connect(); try await finish(model)
            instant = date.addingTimeInterval(91)
            let calls = await service.snapshot()
            check(calls.logins == 1 && model.isOnline && model.campusVerificationState == "confirmed"
                  && model.memoryVerificationMessage == nil && model.cooldownRemaining == nil,
                  "A later explicit fake login supplies fresh proof and clears obsolete service-only feedback")
            check(model.readiness.title == "ready.title.ready" && model.readiness.tone == "success"
                  && model.campusVerificationHintKey == "campus.verify.confirmedHint",
                  "Fresh confirmed campus proof supplies the current readiness and verification hint")
        }

        print("NetworkMemoryAppModelTests: \(assertions) assertions passed")
    }
}
