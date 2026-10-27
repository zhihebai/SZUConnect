import Foundation

/// The route is part of each fake operation, so a borrowed memory cannot pass merely
/// by choosing the correct display label while transport remains on another network.
private actor KnownNetworkService: ConnectionService {
    struct Attempt: Sendable {
        let area: CampusArea
        let route: String?
    }
    struct Snapshot: Sendable {
        let bindings: [String?]
        let probes: [String?]
        let discoveries: [CampusArea]
        let discoveryRoutes: [String?]
        let attempts: [Attempt]
    }
    private var binding: CampusPhysicalRoute?
    private var bindings: [String?] = []
    private var probes: [String?] = []
    private var discoveries: [CampusArea] = []
    private var discoveryRoutes: [String?] = []
    private var attempts: [Attempt] = []
    private var online = false
    private var bindingAccepted = true
    private var discovery = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private var outcome: LoginOutcome = .success
    private var pauseDiscovery = false
    private var discoveryContinuation: CheckedContinuation<Void, Never>?
    private var completedDiscoveries = 0

    func configure(bindingAccepted: Bool = true, discovery: CampusDiscovery? = nil,
                   outcome: LoginOutcome = .success, gated: Bool = false) {
        self.bindingAccepted = bindingAccepted
        if let discovery { self.discovery = discovery }
        self.outcome = outcome
        pauseDiscovery = gated
    }
    func setOnline() { online = true }
    func resetConnections() async { binding = nil }
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool {
        bindings.append(route?.interfaceName)
        guard bindingAccepted else { return false }
        binding = route
        return true
    }
    func probe() async -> ConnectivityReport {
        probes.append(binding?.interfaceName)
        return ConnectivityReport(apple: online, microsoft: online)
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(area)
        discoveryRoutes.append(binding?.interfaceName)
        if pauseDiscovery {
            pauseDiscovery = false
            // Deliberately resumes after cancellation; production must reject obsolete proof.
            await withCheckedContinuation { discoveryContinuation = $0 }
        }
        completedDiscoveries += 1
        return discovery
    }
    func isWaitingForDiscovery() -> Bool { discoveryContinuation != nil }
    func completedDiscoveryCount() -> Int { completedDiscoveries }
    func releaseDiscovery() {
        guard let continuation = discoveryContinuation else { fatalError("No discovery is gated") }
        discoveryContinuation = nil
        continuation.resume()
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        guard area != .automatic else { fatalError("A bound authentication attempt must select one explicit area") }
        attempts.append(Attempt(area: area, route: binding?.interfaceName))
        if outcome == .success { online = true }
        return LoginAttempt(outcome: outcome)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        fatalError("Known-network recovery must acquire fresh staged campus proof")
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        fatalError("Known-network recovery must use a classified single-area attempt")
    }
    func snapshot() -> Snapshot {
        Snapshot(bindings: bindings, probes: probes, discoveries: discoveries,
                 discoveryRoutes: discoveryRoutes, attempts: attempts)
    }
}

private actor KnownGatedWebsite: WebsiteChecking {
    private var continuation: CheckedContinuation<Void, Never>?
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        await withCheckedContinuation { continuation = $0 }
        return .response(200, url: URL(string: input)!, strategy: strategy, elapsed: 0.01)
    }
    func waiting() -> Bool { continuation != nil }
    func release() { let pending = continuation; continuation = nil; pending?.resume() }
}

@MainActor private final class KnownNetworkClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct KnownNetworkAutomationAppModelTests {
    private static var assertions = 0
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let wifi = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:83:19:01", "gwip": "172.29.83.1", "net": "172.29.83.0/24",
        "dhcp": "172.29.83.2", "dns": "172.29.83.3", "sfx": "known-campus-fixture.invalid"
    ])
    private static let wired = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:83:19:02", "gwip": "172.28.84.1", "net": "172.28.84.0/24",
        "dhcp": "172.28.84.2", "dns": "172.28.84.3"
    ])
    private static let unknown = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:83:19:03", "gwip": "172.27.85.1", "net": "172.27.85.0/24"
    ])
    private static let fullProof = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let teachingProof = CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)

    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        assertions += 1
        print("PASS: \(description)")
    }
    private static func folder() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("szu-known-network-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    private static func route(_ descriptor: NetworkDescriptor, name: String, primary: Bool,
                              lease: String = "A") -> CampusPhysicalRoute {
        CampusPhysicalRoute(interfaceName: name, interfaceIndex: name == "en0" ? 1 : 12,
            addresses: [name == "en0" ? "172.29.83.42" : "172.28.84.42"],
            dnsServers: [name == "en0" ? "172.29.83.3" : "172.28.84.3"], identity: "fake-route-\(name)-\(lease)",
            serviceID: "fake-service-\(name)", descriptor: descriptor, isSystemPrimary: primary)
    }
    private static func observation(_ candidates: [CampusPhysicalRoute], tunnel: Bool = true,
                                    fingerprint: String = "topology-A", usable: Bool = true) -> PathObservation {
        var continuity = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: fingerprint), linkActive: true)
        continuity.tunnel = tunnel
        continuity.physicalLabels = ["有线 en12"]
        continuity.systemPhysicalLabel = candidates.first(where: \.isSystemPrimary).map { "physical \($0.interfaceName)" }
        continuity.connectedPhysicalLabels = candidates.map { "physical \($0.interfaceName)" }
        continuity.hasUsablePhysicalLink = true
        continuity.sessionOnlyTrust = true
        continuity.requiresPhysicalBinding = true
        return PathObservation(usable: usable, interfaces: "fixture physical combination", constrained: false,
                               continuity: continuity, physicalCandidates: candidates)
    }
    @discardableResult private static func seed(_ store: URL, wifi: Bool = true, wired: Bool = true,
                                                teachingOnly: Bool = false, age: TimeInterval = 3_600) throws -> Data {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        if wifi {
            guard memory.remember(self.wifi, area: teachingOnly ? "teaching" : "dormitory",
                proofTeaching: true, proofDormitory: !teachingOnly, now: date.addingTimeInterval(-age), newID: "a183abcd") != nil
            else { fatalError("Invalid Wi-Fi memory fixture") }
        }
        if wired {
            guard memory.remember(self.wired, area: teachingOnly ? "teaching" : "dormitory",
                proofTeaching: true, proofDormitory: !teachingOnly, now: date.addingTimeInterval(-age), newID: "b184abcd") != nil
            else { fatalError("Invalid wired memory fixture") }
        }
        let bytes = memory.encoded()
        try NetworkMemoryFile.write(bytes, to: store)
        return bytes
    }
    private static func fixture(store: URL, service: KnownNetworkService, clock: KnownNetworkClock,
                                recoveryStore: URL? = nil, area: CampusArea = .dormitory,
                                checker: (any WebsiteChecking)? = nil, targets: [WebsiteTarget] = []) -> AppModel {
        let model = AppModel(client: service, isolated: true, now: { clock.date }, recoveryStore: recoveryStore,
            isolatedCredentials: Credentials(account: "known-campus-test-account", password: "known-campus-test-password"),
            websiteChecker: checker, initialWebsiteTargets: targets, memoryStore: store)
        model.area = area
        model.setMonitoringMode(.recover)
        return model
    }
    private static func recover(_ model: AppModel) async {
        await model.runAutomaticCycle()
        await model.runAutomaticCycle()
    }
    private static func waitUntil(_ description: String, _ condition: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(description)")
    }

    static func main() async throws {
        setbuf(stdout, nil)
        // Independently remembered physical records retain eligibility across topology changes.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let model = fixture(store: store, service: service, clock: clock)
            model.applyEnvironmentObservation(observation([
                route(wifi, name: "en0", primary: false), route(wired, name: "en12", primary: true)
            ]))
            check(model.networkRemembered, "A remembered physical primary remains remembered with Wi-Fi, Ethernet and TUN attached")
            check(!model.sessionOnlyTrust && !model.currentNetworkAuthorized,
                  "Physical memory removes composite session scope without inventing a new user trust decision")
            await model.runAutomaticCycle()
            check(await service.snapshot().attempts.isEmpty, "One failed automatic probe cannot submit credentials")
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(snapshot.attempts.count == 1 && snapshot.attempts.first?.area == .dormitory,
                  "Two fresh failures plus campus proofs reconnect automatically without user actions")
            check(!snapshot.bindings.isEmpty && snapshot.bindings.allSatisfy { $0 == "en12" }
                  && snapshot.probes.allSatisfy { $0 == "en12" }
                  && snapshot.discoveryRoutes == ["en12"] && snapshot.attempts.first?.route == "en12",
                  "Memory, preflight, campus proof and credential submission all use the same selected physical route")
            check(model.isOnline && !model.currentNetworkAuthorized,
                  "Successful remembered-network recovery never synthesizes session authorization")
        }

        // Reopening, a DHCP lease renewal, adding another physical link, and changing
        // tunnel status change operation identity while preserving known-network eligibility.
        for variant in ["restart", "lease", "tun-off", "extra-link", "wake"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            var model = fixture(store: store, service: service, clock: clock)
            let first = observation([route(wired, name: "en12", primary: true)])
            model.applyEnvironmentObservation(first)
            check(model.networkRemembered && !model.currentNetworkAuthorized, "\(variant): initial known network requires no repeated trust action")
            if variant == "restart" { model = fixture(store: store, service: service, clock: clock) }
            if variant == "wake" { model.setSleeping(true); model.setSleeping(false) }
            let candidates = [route(wired, name: "en12", primary: true, lease: variant == "lease" ? "B" : "A")]
                + (variant == "extra-link" ? [route(unknown, name: "en0", primary: false)] : [])
            model.applyEnvironmentObservation(observation(candidates, tunnel: variant != "tun-off", fingerprint: "topology-\(variant)"))
            check(model.networkRemembered && !model.sessionOnlyTrust && !model.currentNetworkAuthorized,
                  "\(variant): a fresh attachment reuses only stored physical eligibility")
            check(model.presentation.banner?.key != "banner.sessionTrustOnly" && model.presentation.primary != "trust",
                  "\(variant): an eligible remembered physical network does not display the composite trust request")
            check(try Data(contentsOf: store) == original,
                  "\(variant): recognition neither overwrites memory nor creates a remembered connection combination")
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.attempts.count == 1 && snapshot.attempts.first?.route == "en12" && model.isOnline,
                  "\(variant): fresh probes and campus proof recover automatically on the known physical network")
        }

        // A reliable system primary wins without borrowing a remembered peer's proof.
        // Without a primary, a known secondary remains a deterministic fallback.
        for (hasPrimary, reverse) in [(true, false), (true, true), (false, false), (false, true)] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, wired: false)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let model = fixture(store: store, service: service, clock: clock)
            let candidates = [route(unknown, name: "en12", primary: hasPrimary), route(wifi, name: "en0", primary: false)]
            model.applyEnvironmentObservation(observation(reverse ? Array(candidates.reversed()) : candidates))
            check(model.networkRemembered == !hasPrimary && !model.currentNetworkAuthorized,
                  "Known secondary primary=\(hasPrimary) reverse=\(reverse): only the selected attachment can supply memory")
            await recover(model)
            let snapshot = await service.snapshot()
            if hasPrimary {
                check(snapshot.attempts.isEmpty,
                      "An unknown system primary cannot inherit credentials admission from its remembered Wi-Fi peer")
                check(!snapshot.bindings.isEmpty && snapshot.bindings.allSatisfy { $0 == "en12" }
                      && snapshot.probes.allSatisfy { $0 == "en12" } && snapshot.discoveryRoutes.allSatisfy { $0 == "en12" },
                      "Reordered candidates retain the unknown primary's independent probe and service scope")
            } else {
                check(snapshot.attempts.count == 1 && snapshot.attempts.first?.route == "en0",
                      "Without a primary, fallback credentials use only the remembered physical Wi-Fi")
                check(!snapshot.bindings.isEmpty && snapshot.bindings.allSatisfy { $0 == "en0" }
                      && snapshot.probes.allSatisfy { $0 == "en0" } && snapshot.discoveryRoutes == ["en0"],
                      "Fallback probes and campus proof retain the same remembered physical binding")
            }
        }

        // No physical proof, a changed gateway, missing memory, and a revoked record
        // remain ineligible even though the interface is up and campus pages look valid.
        for variant in ["missing-route", "unknown", "conflict", "forgotten", "expired", "corrupt-memory"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, wifi: false, age: variant == "expired" ? NetworkMemory.expiry + 1 : 3_600)
            if variant == "corrupt-memory" { try Data("unreadable-memory-fixture".utf8).write(to: store) }
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let model = fixture(store: store, service: service, clock: clock)
            var descriptor = wired
            if variant == "conflict" { descriptor.raw["gw"] = "a4:5e:60:83:ff:ff" }
            if variant == "unknown" { descriptor = unknown }
            let candidate = route(descriptor, name: "en12", primary: true)
            let candidates = variant == "missing-route" ? [] : [candidate]
            model.applyEnvironmentObservation(observation(candidates))
            if variant == "forgotten" {
                check(model.networkRemembered, "Forget fixture first recognizes the current remembered network")
                model.forgetCurrentNetwork()
                model.applyEnvironmentObservation(observation(candidates, fingerprint: "after-forgetting"))
            }
            check(!model.networkRemembered && !model.currentNetworkAuthorized,
                  "\(variant): no remembered physical authority is inferred from topology or campus availability")
            await recover(model)
            check(await service.snapshot().attempts.isEmpty,
                  "\(variant): automatic cycles submit zero credentials without an eligible physical network")
            if variant == "corrupt-memory" {
                let preserved = try Data(contentsOf: store)
                check(model.memoryUnreadable && preserved == Data("unreadable-memory-fixture".utf8),
                      "Unreadable remembered-network data is preserved without being replaced")
            }
        }

        // A click queued from the previous UI can arrive after live candidate evidence
        // disappears. A surviving link/fingerprint cannot substitute for a bound route.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, wifi: false)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let model = fixture(store: store, service: service, clock: clock)
            let candidate = route(wired, name: "en12", primary: true)
            model.applyEnvironmentObservation(observation([candidate]))
            check(model.networkRemembered && model.canTrustCurrentNetwork,
                  "Queued-trust fixture begins with a known, physically bound network")
            var stale = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: "physical:" + candidate.identity),
                linkActive: true)
            stale.tunnel = true
            stale.hasUsablePhysicalLink = true
            stale.requiresPhysicalBinding = true
            stale.descriptors = [wired]
            model.applyEnvironmentObservation(PathObservation(usable: true, interfaces: "有线 en12", constrained: false,
                continuity: stale, physicalCandidates: []))
            check(!model.canTrustCurrentNetwork && !model.networkRemembered,
                  "Missing live physical candidates invalidate trust eligibility despite a surviving link and fingerprint")
            model.authorizeCurrentNetwork()
            check(!model.currentNetworkAuthorized,
                  "A trust click queued before candidate loss cannot authorize an unbound physical network")
            await recover(model)
            check(await service.snapshot().attempts.isEmpty && !model.currentNetworkAuthorized,
                  "Automatic recovery after physical evidence loss submits zero credentials after a stale trust click")
        }

        // Failed disk revocation must still suppress that physical record for this session.
        do {
            let root = try folder()
            defer {
                try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: root.path)
                try? FileManager.default.removeItem(at: root)
            }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store, wifi: false)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let model = fixture(store: store, service: service, clock: clock)
            let candidates = [route(wired, name: "en12", primary: true)]
            model.applyEnvironmentObservation(observation(candidates))
            try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: root.path)
            model.forgetCurrentNetwork()
            model.applyEnvironmentObservation(observation(candidates, fingerprint: "return-after-failed-forget"))
            check(model.memoryWriteFailed && !model.networkRemembered,
                  "Failed forget suppresses the physical record even when its old memory bytes remain on disk")
            check(try Data(contentsOf: store) == original, "Failed forget preserves the previous stored file exactly")
            await recover(model)
            check(await service.snapshot().attempts.isEmpty,
                  "A network candidate cannot resurrect eligibility after a failed disk revocation")
        }

        // Route rejection is checked before even acquiring campus proof; saved memory
        // is no permission to use whichever interface transport happens to choose.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            await service.configure(bindingAccepted: false)
            let model = fixture(store: store, service: service, clock: clock)
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)]))
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.attempts.isEmpty && snapshot.discoveries.isEmpty && snapshot.probes.isEmpty,
                  "A physical-route binding mismatch stops probes, campus discovery and credential submission")
            check(!model.currentNetworkAuthorized, "Rejected transport binding cannot grant session trust")
        }

        // Physical recognition does not replace this round's required campus proof.
        // Retain the existing dual-proof safeguard when a TUN is active.
        for variant in ["missing-teaching", "missing-dormitory", "missing-both"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let proof = CampusDiscovery(teaching: variant == "missing-dormitory" ? .confirmed : .unconfirmed,
                                        dormitory: variant == "missing-teaching" ? .confirmed : .unconfirmed)
            await service.configure(discovery: proof)
            let model = fixture(store: store, service: service, clock: clock)
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)]))
            await recover(model)
            let snapshot = await service.snapshot()
            check(model.networkRemembered && snapshot.discoveries == [.dormitory],
                  "\(variant): stored physical eligibility still triggers fresh campus discovery")
            check(snapshot.attempts.isEmpty && !model.currentNetworkAuthorized,
                  "\(variant): incomplete current campus proof cannot submit credentials under TUN")
        }

        // A stored hold or previous credential submission always outranks recognition.
        for variant in ["userPaused", "credentials", "security", "uncertain", "cooldown"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let journal = root.appendingPathComponent("recovery.json")
            _ = try seed(store)
            let reason: HoldReason? = variant == "userPaused" ? .userPaused : variant == "credentials" ? .credentials
                : variant == "security" ? .security : variant == "uncertain" ? .uncertain : nil
            let holds = reason.map { [AuthenticationHold(reason: $0, generation: $0 == .credentials ? 1 : nil,
                origin: $0 == .userPaused ? .user : .global, rejection: $0 == .credentials ? "password" : "")] } ?? []
            try RecoveryJournal(generation: 1, holds: holds, notBefore: variant == "cooldown" ? date.addingTimeInterval(90) : nil,
                                pendingSubmission: variant == "uncertain").write(journal)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            let model = fixture(store: store, service: service, clock: clock, recoveryStore: journal)
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)]))
            check(model.networkRemembered, "\(variant): remembered eligibility is distinct from permission to submit")
            await recover(model)
            check(await service.snapshot().attempts.isEmpty && !model.currentNetworkAuthorized,
                  "\(variant): physical recognition never clears an authentication hold or submission cooldown")
            if reason != nil { check(model.holds == holds, "\(variant): stored authentication holds survive automatic recognition") }
        }

        // Unrelated TUN/UI path changes must not cancel a live, physically bound operation.
        for change in ["tun-off", "aux-link", "primary-metadata", "global-unavailable"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            await service.configure(gated: true)
            let model = fixture(store: store, service: service, clock: clock)
            let original = route(wired, name: "en12", primary: true)
            model.applyEnvironmentObservation(observation([original]))
            await model.runAutomaticCycle()
            let pending = Task { await model.runAutomaticCycle() }
            try await waitUntil("physical discovery before unrelated change") { await service.isWaitingForDiscovery() }
            var selected = original
            if change == "primary-metadata" { selected.isSystemPrimary = false }
            let candidates = [selected] + (change == "aux-link" ? [route(wifi, name: "en0", primary: false)] : [])
            model.applyEnvironmentObservation(observation(candidates, tunnel: change != "tun-off",
                fingerprint: "unrelated-" + change, usable: change != "global-unavailable"))
            check(model.busy && model.networkRemembered,
                  "\(change): current physical operation and remembered eligibility survive unrelated global changes")
            await service.releaseDiscovery(); await pending.value
            check(await service.snapshot().attempts.count == 1 && model.isOnline,
                  "\(change): fresh proof completes on the same physical route without restarting or manual input")
        }

        // TUN route churn invalidates website replies without cancelling the bound base probe.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock(), checker = KnownGatedWebsite()
            await service.setOnline()
            let model = fixture(store: store, service: service, clock: clock, checker: checker,
                targets: [WebsiteTarget(id: "gated", url: "https://gated.fixture.invalid/")])
            model.setProbeMode(.complete)
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)], fingerprint: "TUN-A"))
            await model.runAutomaticCycle() // First quick success schedules the full read-only supplement.
            let pending = Task { await model.runAutomaticCycle() }
            try await waitUntil("website on old TUN path") { await checker.waiting() }
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)], fingerprint: "TUN-B"))
            check(model.busy && model.networkRemembered, "TUN route churn keeps the bound operation alive")
            await checker.release(); await pending.value
            check(model.isOnline && model.lastInternetCheck?.completeBasePassed == true,
                  "TUN route churn preserves fresh physical connectivity proof")
            check(model.lastInternetCheck?.websites.isEmpty == true && model.lastInternetCheck?.verdict != .allPassed,
                  "Late successful website reply from old TUN path cannot be displayed as current success")
            check(await service.snapshot().attempts.isEmpty && model.recommendedCheckDelay == 2,
                  "TUN churn schedules one read-only website supplement without campus reauthentication")
        }

        // Teaching-only memory must remain an HTTPS-only path, even if a fresh dormant
        // HTTP endpoint is present and even when the HTTPS preflight cannot complete.
        for unavailable in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, wifi: false, teachingOnly: true)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            await service.configure(discovery: fullProof, outcome: unavailable ? .unavailable : .success)
            let model = fixture(store: store, service: service, clock: clock, area: .automatic)
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)]))
            check(model.networkRemembered && model.dormitoryMemoryLevel == .none,
                  "Teaching-only \(unavailable): physical recognition grants no HTTP dormitory authority")
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.discoveries == [.teaching] && snapshot.attempts.map(\.area) == [.teaching],
                  "Teaching-only \(unavailable): automatic choice stays on HTTPS and never falls back to dormitory")
            check(model.dormitoryMemoryLevel == .none && !model.currentNetworkAuthorized,
                  "Teaching-only \(unavailable): fresh page proof cannot silently upgrade saved HTTP consent")
            let memory = NetworkMemory.decode(try Data(contentsOf: store), now: date)
            check(memory?.records.count == 1 && memory?.records.first?.proofDormitory == false,
                  "Teaching-only \(unavailable): automatic recovery preserves the stored proof level")
        }

        // Changing route, forgetting the network, or sleeping during campus discovery
        // invalidates that proof even when the fake discovery ignores cancellation.
        for action in ["route", "forget", "sleep"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = KnownNetworkService(), clock = KnownNetworkClock()
            await service.configure(gated: true)
            let model = fixture(store: store, service: service, clock: clock)
            model.applyEnvironmentObservation(observation([route(wired, name: "en12", primary: true)]))
            await model.runAutomaticCycle()
            let pending = Task { await model.runAutomaticCycle() }
            try await waitUntil("gated bound campus discovery") { await service.isWaitingForDiscovery() }
            switch action {
            case "forget": model.forgetCurrentNetwork()
            case "sleep": model.setSleeping(true)
            default: model.applyEnvironmentObservation(observation([route(wifi, name: "en0", primary: true)], fingerprint: "changed-to-wifi"))
            }
            await service.releaseDiscovery()
            await pending.value
            let completedDiscoveries = await service.completedDiscoveryCount()
            let afterChangedRoute = await service.snapshot()
            check(completedDiscoveries == 1 && afterChangedRoute.attempts.isEmpty,
                  "\(action): late campus proof from an obsolete physical context cannot submit any credentials")
            check(!model.currentNetworkAuthorized, "\(action): cancelling an obsolete context never grants session trust")
            if action == "route" {
                check(model.networkRemembered, "A changed physical route independently recognizes the new network's saved record")
                await recover(model)
                let snapshot = await service.snapshot()
                check(snapshot.attempts.count == 1 && snapshot.attempts.first?.route == "en0",
                      "The new known route needs its own two fresh failures and campus proof before automatic login")
            } else if action == "forget" {
                check(!model.networkRemembered, "Forgetting while campus proof is pending keeps the previous record revoked")
                await recover(model)
                check(await service.snapshot().attempts.isEmpty, "Forgotten network remains blocked after an obsolete proof completes")
            }
        }
        print("KnownNetworkAutomationAppModelTests: \(assertions) assertions passed; no live network or Keychain used")
    }
}
