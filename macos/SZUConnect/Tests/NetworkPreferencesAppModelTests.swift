import Foundation
import Darwin

/// A name or Wi-Fi preference is local metadata. Any unexpected campus operation is counted;
/// all replies are in RAM and contain no real credentials or network addresses.
private actor PreferencesConnectionService: ConnectionService {
    struct Calls: Equatable, Sendable {
        var probes = 0
        var discoveries = 0
        var logins = 0
        var resets = 0
    }
    private var calls = Calls()
    func resetConnections() async { calls.resets += 1 }
    func probe() async -> ConnectivityReport {
        calls.probes += 1
        return ConnectivityReport(apple: true, microsoft: true)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        calls.discoveries += 1
        return .dormitory
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        calls.discoveries += 1
        return CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        calls.logins += 1
        return "fixture-only"
    }
    func snapshot() -> Calls { calls }
}

@MainActor @main
struct NetworkPreferencesAppModelTests {
    private static var checks = 0
    private static var failures: [String] = []
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let firstID = "01234567"
    private static let secondID = "89abcdef"
    private nonisolated static let ssid = "Private-Campus-WiFi-24"
    private static let personalName = "私人宿舍网络名称-24"
    private nonisolated static let descriptor = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:24:01:01", "gwip": "172.29.24.1", "net": "172.29.24.0/24"
    ])
    private static let secondDescriptor = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:24:02:01", "gwip": "172.28.24.1", "net": "172.28.24.0/24"
    ])

    private struct Stores {
        let directory: URL
        var memory: URL { directory.appendingPathComponent("network-memory.json") }
        var labels: URL { directory.appendingPathComponent(NetworkLabelsStore.filename) }
        var recovery: URL { directory.appendingPathComponent("recovery.json") }
        init() throws {
            directory = FileManager.default.temporaryDirectory.appendingPathComponent("szu-network-preferences-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        func remove() { try? FileManager.default.removeItem(at: directory) }
    }

    private static func expect(_ condition: @autoclosure () throws -> Bool, _ message: String) {
        checks += 1
        do {
            if try condition() { print("PASS: \(message)") }
            else { failures.append(message); print("FAIL: \(message)") }
        } catch { failures.append("\(message): \(error)"); print("FAIL: \(message): \(error)") }
    }

    @discardableResult
    private static func seed(_ stores: Stores) throws -> Data {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        guard memory.remember(descriptor, area: "dormitory", proofTeaching: true, proofDormitory: true,
                              now: date.addingTimeInterval(-3_600), newID: firstID) != nil,
              memory.remember(secondDescriptor, area: "teaching", proofTeaching: true, proofDormitory: true,
                              now: date.addingTimeInterval(-7_200), newID: secondID) != nil else {
            fatalError("Invalid remembered-network fixtures")
        }
        let bytes = memory.encoded()
        try NetworkMemoryFile.write(bytes, to: stores.memory)
        return bytes
    }

    private static func seedLabels(_ stores: Stores) throws -> Data {
        var labels = NetworkLabels()
        labels.updateAutomatic(id: firstID, displayName: ssid, kind: .wifi, ssid: ssid)
        let bytes = labels.encoded()
        try bytes.write(to: stores.labels)
        return bytes
    }

    private static func model(_ stores: Stores, service: PreferencesConnectionService = PreferencesConnectionService(),
                              readOnly: Bool = false, recovery: Bool = false) -> AppModel {
        AppModel(client: service, isolated: true, readOnly: readOnly, now: { date },
                 recoveryStore: recovery ? stores.recovery : nil,
                 isolatedCredentials: Credentials(account: "fixture-account-24", password: "fixture-password-24"),
                 memoryStore: stores.memory, labelsStore: stores.labels)
    }

    private static func attach(_ model: AppModel, identity: NetworkIdentity? = NetworkIdentity(fingerprint: "fixture-identity-24"),
                               usable: Bool = true, linkActive: Bool = true,
                               descriptors: [NetworkDescriptor] = [descriptor]) {
        model.receivePath(usable: usable, interfaces: "Wi-Fi en0", identity: identity,
                          linkActive: linkActive, now: date, descriptors: descriptors,
                          physicalConnected: usable && linkActive)
    }

    private static func observe(_ model: AppModel, name: String = ssid) {
        model.observeNetworkLabel(.make(kind: .wifi, ssid: name))
    }

    private nonisolated static let wiredDescriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:24:02:01", "gwip": "172.28.24.1", "net": "172.28.24.0/24"
    ])

    @discardableResult
    private static func seedDualAttachments(_ stores: Stores) throws -> Data {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        _ = memory.remember(descriptor, area: "dormitory", proofTeaching: true, proofDormitory: true,
                            now: date.addingTimeInterval(-3_600), newID: firstID)
        _ = memory.remember(wiredDescriptor, area: "teaching", proofTeaching: true, proofDormitory: true,
                            now: date.addingTimeInterval(-7_200), newID: secondID)
        let bytes = memory.encoded()
        try NetworkMemoryFile.write(bytes, to: stores.memory)
        return bytes
    }

    private static func dualObservation(name: String = ssid, knownEgress: Bool = true,
                                        physical: Bool = true,
                                        wifiDescriptor: NetworkDescriptor = descriptor,
                                        wifiLabel: NetworkObservedLabel? = nil) -> PathObservation {
        var snapshot = NetworkContinuitySnapshot(identity: knownEgress ? NetworkIdentity(fingerprint: "fixture-wired-egress-25") : nil,
                                                  linkActive: knownEgress)
        snapshot.hasUsablePhysicalLink = physical
        snapshot.physicalLabels = ["有线 en12", "Wi-Fi en0"]
        snapshot.descriptors = knownEgress ? [wiredDescriptor] : []
        snapshot.wifiLabelAttachments = [.init(descriptor: wifiDescriptor,
                                               label: wifiLabel ?? .make(kind: .wifi, ssid: name))]
        return PathObservation(usable: true, interfaces: "有线 en12 · Wi-Fi en0", constrained: false, continuity: snapshot)
    }

    static func main() async throws {
        setbuf(stdout, nil)

        // A labels sidecar is optional. Loading an older memory file must not rewrite its contents.
        do {
            let stores = try Stores(); defer { stores.remove() }
            let oldBytes = try seed(stores)
            let oldRecords = NetworkMemory.decode(oldBytes, now: date)!.records
            let app = model(stores)
            expect(app.rememberedNetworks == oldRecords && app.rememberedNetworks.count == 2,
                   "Legacy hash-only memory loads every record and proof unchanged")
            expect(!app.memoryUnreadable && !app.networkLabelsUnavailable, "Missing labels sidecar is a supported legacy state")
            expect(try Data(contentsOf: stores.memory) == oldBytes, "Loading legacy memory preserves original bytes")
            expect(!FileManager.default.fileExists(atPath: stores.labels.path), "Loading legacy memory does not invent or write network names")
            expect(app.hasUnnamedRememberedNetworks && app.networkLabels.displayName(for: firstID) == NetworkLabels.unnamed,
                   "Legacy records expose an honest unnamed state instead of numbered or guessed names")
            expect(app.wifiRecovery.allowedNetworks.isEmpty && app.wifiRecovery.selectedNetworkIDs.isEmpty,
                   "Hash-only legacy records are not implicit Wi-Fi candidates or selections")
            attach(app)
            expect(app.networkRemembered && !app.currentNetworkAuthorized && app.memoryVerdictNow == .awaitingProof,
                   "Missing names do not change memory eligibility or grant session trust")

            observe(app)
            expect(app.networkLabels.entries[firstID]?.displayName == ssid && app.networkLabels.entries[firstID]?.ssid == ssid,
                   "Reliable current network receives its observed Wi-Fi name and exact SSID")
            expect(app.networkLabels.entries[secondID] == nil, "A different remembered hash is never filled with the current network's name")
            expect(try app.rememberedNetworks == oldRecords && Data(contentsOf: stores.memory) == oldBytes,
                   "Observing a name leaves memory IDs, hashes, proofs and verification times unchanged")
            expect(app.wifiRecovery.allowedNetworks.map(\.id) == [firstID] && app.wifiRecovery.allowedNetworks.first?.ssid == ssid,
                   "Observed current Wi-Fi becomes the sole eligible candidate")
            expect(!app.wifiRecovery.autoJoinEnabled && !app.wifiRecovery.powerOnWhenDisconnected,
                   "Automatic Wi-Fi joining and power-on both default off")
            expect(app.wifiRecovery.selectedNetworkIDs == [firstID], "An explicitly remembered Wi-Fi is eligible by default without a second selection step")
            let restored = model(stores)
            expect(restored.networkLabels == app.networkLabels && restored.rememberedNetworks == oldRecords,
                   "Sidecar round trip preserves names and all old memory records")
            expect(restored.wifiRecovery.allowedNetworks.map(\.id) == [firstID], "Reload publishes Wi-Fi candidates after both stores are loaded")
        }

        // Metadata may only be attached to the one reliably identified live physical network.
        for invalidPath in ["unknown identity", "disconnected link", "unavailable path", "weak descriptor", "ambiguous physical paths"] {
            let stores = try Stores(); defer { stores.remove() }
            let bytes = try seed(stores)
            let app = model(stores)
            switch invalidPath {
            case "unknown identity": attach(app, identity: nil)
            case "disconnected link": attach(app, usable: false, linkActive: false)
            case "unavailable path": attach(app, usable: false)
            case "weak descriptor": attach(app, descriptors: [NetworkDescriptor(kind: "wifi", raw: ["gwip": "172.29.24.1"])])
            default: attach(app, descriptors: [descriptor, secondDescriptor])
            }
            observe(app)
            expect(app.networkLabels.entries.isEmpty, "Name binding is refused for \(invalidPath)")
            expect(!FileManager.default.fileExists(atPath: stores.labels.path), "No labels file is written for \(invalidPath)")
            expect(try Data(contentsOf: stores.memory) == bytes, "Memory remains intact for \(invalidPath)")
        }

        // Renaming is presentation-only even with live trust, cooldown and durable safety holds.
        do {
            let stores = try Stores(); defer { stores.remove() }
            let oldMemory = try seed(stores)
            _ = try seedLabels(stores)
            let journal = RecoveryJournal(generation: 1,
                holds: [AuthenticationHold(reason: .userPaused, origin: .user), AuthenticationHold(reason: .security, origin: .global)],
                notBefore: date.addingTimeInterval(63), pendingSubmission: false)
            let oldRecovery = try journal.encoded()
            try oldRecovery.write(to: stores.recovery)
            let service = PreferencesConnectionService()
            let app = model(stores, service: service, recovery: true)
            attach(app)
            app.setMonitoringMode(.recover)
            app.authorizeCurrentNetwork()
            let records = app.rememberedNetworks
            let holds = app.holds
            let cooldown = app.cooldownRemaining
            let authorized = app.currentNetworkAuthorized
            let oldCalls = await service.snapshot()
            let oldEvents = app.events
            expect(authorized && !holds.isEmpty && cooldown == 63, "Rename fixture carries session trust, persistent holds and active cooldown")
            expect(app.renameRememberedNetwork(id: firstID, name: personalName), "A remembered network can be renamed")
            expect(app.networkLabels.entries[firstID]?.displayName == personalName && app.networkLabels.entries[firstID]?.manualName == true,
                   "Rename changes only its local display name and marks the explicit choice")
            expect(app.networkLabels.entries[firstID]?.ssid == ssid, "A custom display name never becomes the connection SSID")
            expect(try app.rememberedNetworks == records && Data(contentsOf: stores.memory) == oldMemory,
                   "Rename preserves memory IDs, features, proof flags, dates and file bytes")
            expect(app.currentNetworkAuthorized == authorized && app.holds == holds && app.cooldownRemaining == cooldown,
                   "Rename preserves session trust, pauses, safety holds and cooldown")
            expect(try Data(contentsOf: stores.recovery) == oldRecovery, "Rename leaves the recovery journal byte-for-byte unchanged")
            expect(app.events.map(\.message) == oldEvents.map(\.message), "Rename does not add private names to the event stream")
            let newCalls = await service.snapshot()
            expect(newCalls == oldCalls && newCalls == PreferencesConnectionService.Calls(),
                   "Rename triggers no probe, campus discovery, login or transport reset")
            expect(app.wifiRecovery.allowedNetworks.first?.displayName == personalName,
                   "The candidate display name follows a manual rename")
            attach(app)
            observe(app)
            expect(app.networkLabels.displayName(for: firstID) == personalName && app.networkLabels.entries[firstID]?.ssid == ssid,
                   "Repeated path and Wi-Fi metadata refresh preserve a manual name")
            app.observeNetworkLabel(.make(kind: .wifi))
            expect(app.networkLabels.displayName(for: firstID) == personalName && app.networkLabels.entries[firstID]?.ssid == ssid,
                   "A later unavailable SSID cannot erase a known name or connection candidate")
            expect(!app.renameRememberedNetwork(id: "fedcba98", name: "orphan-name"), "Rename rejects IDs that are not remembered")
            expect(!app.renameRememberedNetwork(id: firstID, name: " \n "), "Rename rejects a blank name without changing metadata")
            let exported = String(decoding: app.diagnosticExportData, as: UTF8.self)
            let summary = app.diagnosticSummary
            let events = app.events.map(\.message).joined(separator: "\n")
            let forbidden = [ssid, personalName, firstID, secondID, "fixture-account-24", "fixture-password-24"]
            expect(forbidden.allSatisfy { !exported.contains($0) && !summary.contains($0) && !events.contains($0) },
                   "SSID, custom names, record IDs and credentials stay out of exported diagnostics, summaries and events")
            expect((try JSONSerialization.jsonObject(with: app.diagnosticExportData)) is [String: Any],
                   "Diagnostics remain valid JSON after naming and Wi-Fi candidate publication")
        }

        // Read-only launches can display old metadata but cannot mutate or perform Wi-Fi recovery.
        for existingLabels in [false, true] {
            let stores = try Stores(); defer { stores.remove() }
            let memoryBytes = try seed(stores)
            let labelsBytes = existingLabels ? try seedLabels(stores) : nil
            let app = model(stores, readOnly: true)
            attach(app)
            let initialLabels = app.networkLabels
            observe(app, name: "Forbidden-ReadOnly-Update")
            expect(!app.renameRememberedNetwork(id: firstID, name: personalName) && app.networkLabels == initialLabels,
                   "Observe-only refuses observed and manual name mutations (existing labels: \(existingLabels))")
            app.forgetNetwork(id: firstID); app.forgetCurrentNetwork(); app.forgetAllNetworks()
            expect(app.rememberedNetworks.count == 2 && app.networkLabels == initialLabels,
                   "Observe-only refuses all record revocation APIs without changing eligibility or labels")
            expect(try Data(contentsOf: stores.memory) == memoryBytes, "Observe-only preserves legacy memory bytes")
            expect((try? Data(contentsOf: stores.labels)) == labelsBytes, "Observe-only neither creates nor rewrites the labels sidecar")
            expect(!app.wifiRecovery.autoJoinEnabled && !app.wifiRecovery.powerOnWhenDisconnected,
                   "Observe-only starts with Wi-Fi automation and power-on disabled")
            app.setMonitoringMode(.recover)
            app.wifiRecovery.setNetworkSelected(firstID, selected: true)
            app.wifiRecovery.setAutoJoinEnabled(true)
            app.wifiRecovery.setPowerOnWhenDisconnected(true)
            attach(app, identity: nil, usable: false, linkActive: false, descriptors: [])
            await app.wifiRecovery.refreshOnce()
            expect(!app.wifiRecovery.autoJoinEnabled && !app.wifiRecovery.powerOnWhenDisconnected && app.wifiRecovery.status == .disabled, "Observe-only rejects Wi-Fi setting mutations and remains unable to recover after programmatic requests")
        }

        // Each row owns one record, one local label and one Wi-Fi candidate.
        for removedID in [firstID, secondID] {
            let stores = try Stores(); defer { stores.remove() }
            try seed(stores)
            var labels = NetworkLabels()
            labels.updateAutomatic(id: firstID, displayName: ssid, kind: .wifi, ssid: ssid)
            labels.updateAutomatic(id: secondID, displayName: "Other-Campus-WiFi-27", kind: .wifi, ssid: "Other-Campus-WiFi-27")
            try labels.encoded().write(to: stores.labels)
            let app = model(stores)
            attach(app); app.authorizeCurrentNetwork()
            let retainedID = removedID == firstID ? secondID : firstID
            let retainedLabel = app.networkLabels.entries[retainedID]
            expect(app.wifiRecovery.allowedNetworks.count == 2 && app.wifiRecovery.selectedNetworkIDs.count == 2,
                   "Single-row fixture exposes two independent selected Wi-Fi candidates")
            app.forgetNetwork(id: removedID)
            expect(app.rememberedNetworks.map(\.id) == [retainedID], "Single-row forget preserves the other memory record")
            expect(app.networkLabels.entries[removedID] == nil && app.networkLabels.entries[retainedID] == retainedLabel,
                   "Single-row forget removes only its own label and preserves the other exact SSID")
            expect(app.wifiRecovery.allowedNetworks.map(\.id) == [retainedID]
                   && app.wifiRecovery.selectedNetworkIDs == [retainedID],
                   "Single-row forget immediately removes its candidate without deselecting the other Wi-Fi")
            let diskLabels = NetworkLabels.decode(try Data(contentsOf: stores.labels))
            expect(diskLabels?.entries[removedID] == nil && diskLabels?.entries[retainedID] == retainedLabel,
                   "Single-row label pruning persists only the requested removal")
            expect(app.currentNetworkAuthorized == (removedID != firstID),
                   "Forgetting an unrelated row preserves current session trust")
        }

        // Removing a memory revokes its candidate/selection and its name while retaining other records.
        do {
            let stores = try Stores(); defer { stores.remove() }
            try seed(stores)
            let app = model(stores)
            attach(app); observe(app)
            app.wifiRecovery.setNetworkSelected(firstID, selected: true)
            expect(app.wifiRecovery.selectedNetworkIDs == [firstID], "Only explicit selection enables a remembered Wi-Fi candidate")
            app.forgetCurrentNetwork()
            expect(app.rememberedNetworks.map(\.id) == [secondID], "Forgetting current memory preserves the unrelated remembered record")
            expect(app.networkLabels.entries[firstID] == nil && app.wifiRecovery.allowedNetworks.isEmpty && app.wifiRecovery.selectedNetworkIDs.isEmpty,
                   "Forgetting current memory revokes its name, candidate and prior selection")
            attach(app, identity: NetworkIdentity(fingerprint: "fixture-identity-other-24"), descriptors: [secondDescriptor])
            observe(app, name: "Other-Campus-WiFi-24")
            app.wifiRecovery.setNetworkSelected(secondID, selected: true)
            expect(app.wifiRecovery.allowedNetworks.map(\.id) == [secondID], "The remaining remembered network can acquire its own candidate")
            app.forgetAllNetworks()
            expect(app.rememberedNetworks.isEmpty && app.networkLabels.entries.isEmpty && app.wifiRecovery.allowedNetworks.isEmpty
                   && app.wifiRecovery.selectedNetworkIDs.isEmpty, "Forgetting all records removes every name, candidate and selection")
            let restored = model(stores)
            expect(restored.rememberedNetworks.isEmpty && restored.networkLabels.entries.isEmpty && restored.wifiRecovery.allowedNetworks.isEmpty,
                   "Forgotten metadata and Wi-Fi candidates do not reappear after reload")
        }

        // A newer or corrupt labels file is preserved independently of usable legacy network memory.
        for original in [Data(#"{"schema":99,"labels":{"future":"preserve-me"}}"#.utf8), Data("broken-labels-preserve-24".utf8)] {
            let stores = try Stores(); defer { stores.remove() }
            let memoryBytes = try seed(stores)
            try original.write(to: stores.labels)
            let service = PreferencesConnectionService()
            let app = model(stores, service: service)
            attach(app)
            expect(app.networkLabelsUnavailable && !app.memoryUnreadable && app.networkRemembered && app.rememberedNetworks.count == 2,
                   "Unavailable labels disable naming without disabling existing memory eligibility")
            observe(app)
            expect(!app.renameRememberedNetwork(id: firstID, name: personalName), "Unavailable labels refuse manual rewriting")
            expect(try Data(contentsOf: stores.labels) == original, "Unknown or corrupt labels bytes survive observation and rename")
            expect(try Data(contentsOf: stores.memory) == memoryBytes, "Unavailable labels never rewrite the old memory file")
            expect(app.wifiRecovery.allowedNetworks.isEmpty && !app.wifiRecovery.autoJoinEnabled,
                   "Unreadable labels cannot supply an automatic Wi-Fi candidate")
            app.authorizeCurrentNetwork()
            expect(app.currentNetworkAuthorized, "Unavailable names still allow explicit session-only trust")
            app.forgetAllNetworks()
            expect(try app.rememberedNetworks.isEmpty && Data(contentsOf: stores.labels) == original,
                   "Memory revocation remains available and preserves unknown labels bytes")
            let calls = await service.snapshot()
            expect(calls == PreferencesConnectionService.Calls(), "Unavailable labels do not trigger any campus request")
        }

        // An attached Wi-Fi may supply its own name while Ethernet remains the sole egress.
        do {
            let stores = try Stores(); defer { stores.remove() }
            let memoryBytes = try seedDualAttachments(stores)
            var initialNames = NetworkLabels()
            initialNames.updateAutomatic(id: secondID, displayName: "Fixture Wired", kind: .ethernet)
            try initialNames.encoded().write(to: stores.labels)
            let service = PreferencesConnectionService()
            let app = model(stores, service: service)
            let records = app.rememberedNetworks
            let wiredName = app.networkLabels.entries[secondID]
            app.applyEnvironmentObservation(dualObservation())
            expect(app.networkLabels.entries[firstID]?.displayName == ssid && app.networkLabels.entries[firstID]?.ssid == ssid,
                   "Secondary live Wi-Fi fills its own remembered name while Ethernet is the selected egress")
            expect(app.wifiRecovery.allowedNetworks.map(\.id) == [firstID] && app.wifiRecovery.selectedNetworkIDs == [firstID],
                   "The named secondary remembered Wi-Fi is eligible without a second selection step")
            expect(app.networkLabels.entries[secondID] == wiredName, "Wi-Fi name attachment never overwrites the Ethernet label")
            expect(try app.rememberedNetworks == records && Data(contentsOf: stores.memory) == memoryBytes,
                   "Secondary name completion preserves every memory feature, proof, date and original byte")
            expect(app.memoryMatch.record == secondID && app.memoryMatch.area == "teaching" && !app.hasMultipleNetworkAttachments,
                   "Current memory authorization continues to use only the Ethernet egress descriptor")
            expect(!app.currentNetworkAuthorized && app.memoryVerdictNow == .awaitingProof,
                   "A named secondary Wi-Fi grants neither session trust nor fresh campus proof")
            let revision = app.networkRevision
            app.applyEnvironmentObservation(dualObservation(name: "Updated-Fixture-Campus-25"))
            expect(app.networkRevision == revision && app.networkLabels.entries[firstID]?.ssid == "Updated-Fixture-Campus-25",
                   "An identical core path still accepts fresh secondary SSID metadata before observation deduplication")
            expect(app.renameRememberedNetwork(id: firstID, name: personalName), "A secondary remembered Wi-Fi can retain an explicit display name")
            app.applyEnvironmentObservation(dualObservation(name: "Latest-Fixture-Campus-25"))
            expect(app.networkLabels.entries[firstID]?.displayName == personalName
                   && app.networkLabels.entries[firstID]?.manualName == true
                   && app.networkLabels.entries[firstID]?.ssid == "Latest-Fixture-Campus-25",
                   "Secondary SSID refresh updates exact connection metadata while preserving the manual display name")
            expect(try app.memoryMatch.record == secondID && !app.currentNetworkAuthorized
                   && app.networkLabels.entries[secondID] == wiredName && Data(contentsOf: stores.memory) == memoryBytes,
                   "Repeated secondary metadata cannot change the egress match, trust, wired label or memory bytes")
            let calls = await service.snapshot()
            expect(calls == PreferencesConnectionService.Calls(), "Secondary Wi-Fi name completion makes no probe, discovery, login or transport call")
            let exported = String(decoding: app.diagnosticExportData, as: UTF8.self)
            let events = app.events.map(\.message).joined(separator: "\n")
            expect([ssid, personalName, "Latest-Fixture-Campus-25"].allSatisfy { !exported.contains($0) && !events.contains($0) },
                   "Secondary names and SSIDs never enter diagnostics or event messages")
        }

        // The same naming channel is useful when egress is unknown, without lending it trust.
        do {
            let stores = try Stores(); defer { stores.remove() }
            let bytes = try seedDualAttachments(stores)
            let app = model(stores)
            app.applyEnvironmentObservation(dualObservation(knownEgress: false))
            expect(app.networkLabels.entries[firstID]?.ssid == ssid && app.networkLabels.entries[secondID] == nil,
                   "Unknown egress can name the uniquely matched physical Wi-Fi without naming an unrelated record")
            expect(app.memoryMatch.level == .none && !app.networkRemembered && !app.currentNetworkAuthorized
                   && !app.canTrustCurrentNetwork,
                   "Unknown egress with a physical Wi-Fi name remains ineligible for memory authorization and session trust")
            expect(try Data(contentsOf: stores.memory) == bytes && app.wifiRecovery.selectedNetworkIDs == [firstID],
                   "Unknown-egress name completion preserves memory bytes and only supplies a future Wi-Fi candidate")
        }

        // Invalid or protected contexts must neither create a name nor mutate existing files.
        for reason in ["read-only", "sleeping", "labels unreadable", "memory unreadable", "no physical link",
                       "weak descriptor", "wrong descriptor kind", "wrong label kind", "missing SSID", "ambiguous match"] {
            let stores = try Stores(); defer { stores.remove() }
            _ = try seedDualAttachments(stores)
            if reason == "labels unreadable" { try Data("unreadable-label-fixture-25".utf8).write(to: stores.labels) }
            if reason == "memory unreadable" { try Data("unreadable-memory-fixture-25".utf8).write(to: stores.memory) }
            if reason == "ambiguous match" {
                var object = try JSONSerialization.jsonObject(with: Data(contentsOf: stores.memory)) as! [String: Any]
                var records = object["records"] as! [[String: Any]]
                var duplicate = records.first { $0["id"] as? String == firstID }!
                duplicate["id"] = "fedcba98"
                records.append(duplicate); object["records"] = records
                try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]).write(to: stores.memory)
            }
            let memoryBefore = try Data(contentsOf: stores.memory)
            let labelsBefore = try? Data(contentsOf: stores.labels)
            let app = model(stores, readOnly: reason == "read-only")
            if reason == "sleeping" { app.setSleeping(true) }
            let weak = NetworkDescriptor(kind: "wifi", raw: ["gwip": "172.29.24.1"])
            let wrong = NetworkDescriptor(kind: "ethernet", raw: descriptor.raw)
            let observedLabel: NetworkObservedLabel? = reason == "wrong label kind" ? .make(kind: .ethernet, serviceName: "Wrong wired label")
                : reason == "missing SSID" ? .make(kind: .wifi) : nil
            app.applyEnvironmentObservation(dualObservation(physical: reason != "no physical link",
                wifiDescriptor: reason == "weak descriptor" ? weak : reason == "wrong descriptor kind" ? wrong : descriptor,
                wifiLabel: observedLabel))
            expect(app.networkLabels.entries[firstID] == nil && app.wifiRecovery.allowedNetworks.isEmpty,
                   "Secondary Wi-Fi name binding refuses \(reason)")
            expect(try Data(contentsOf: stores.memory) == memoryBefore && (try? Data(contentsOf: stores.labels)) == labelsBefore,
                   "Secondary Wi-Fi observation preserves both files for \(reason)")
        }

        // A failed disk revocation suppresses the record for this process; late name refreshes
        // must not recreate its sidecar or candidate. The failure is a temporary path collision.
        do {
            let stores = try Stores(); defer { stores.remove() }
            let bytes = try seedDualAttachments(stores)
            let app = model(stores)
            attach(app); observe(app)
            let backup = stores.directory.appendingPathComponent("preserved-memory.json")
            try FileManager.default.moveItem(at: stores.memory, to: backup)
            try FileManager.default.createDirectory(at: stores.memory, withIntermediateDirectories: false)
            app.forgetCurrentNetwork()
            expect(app.memoryWriteFailed && app.rememberedNetworks.map(\.id) == [secondID],
                   "Failed memory removal suppresses the forgotten Wi-Fi while preserving the other record")
            try FileManager.default.removeItem(at: stores.memory)
            try FileManager.default.moveItem(at: backup, to: stores.memory)
            app.applyEnvironmentObservation(dualObservation())
            expect(app.networkLabels.entries[firstID] == nil && app.wifiRecovery.allowedNetworks.isEmpty,
                   "A late attached-Wi-Fi observation cannot restore a name or candidate suppressed by failed revocation")
            expect(try Data(contentsOf: stores.memory) == bytes && app.memoryMatch.record == secondID && !app.currentNetworkAuthorized,
                   "Suppressed secondary metadata cannot alter the preserved disk memory or the current Ethernet authority")
        }

        if failures.isEmpty { print("PASS NetworkPreferencesAppModelTests: \(checks) checks") }
        else { print("FAIL NetworkPreferencesAppModelTests: \(failures.count) / \(checks) checks"); exit(1) }
    }
}
