import Foundation
import Darwin

/// Every attachment has its own connectivity and replies. A working Wi-Fi path must
/// never make an unauthenticated Ethernet path appear online in the test fixture.
private actor DualAttachmentService: ConnectionService {
    enum Stage: String, Hashable { case probe, discovery, preflight, attempt, confirmation }
    struct Request: Sendable {
        let stage: Stage
        let interface: String
        let area: CampusArea?
        let submitted: Bool
        var requiresBoundClient = false
        var route: CampusPhysicalRoute? = nil
    }
    struct Reply: Sendable {
        let attempt: LoginAttempt
        let submitted: Bool
        static let accepted = Reply(attempt: .success("Dual attachment fixture accepted"), submitted: true)
    }
    struct Snapshot: Sendable {
        let bindings: [String?]
        let requests: [Request]
        var attempts: [Request] { requests.filter { $0.stage == .attempt } }
        var submissions: [Request] { attempts.filter(\.submitted) }
    }
    private var binding: CampusPhysicalRoute?
    private var availableInterfaces: Set<String> = ["en0", "en12"]
    private var bindings: [String?] = []
    private var requests: [Request] = []
    private var online: [String: Bool] = ["en0": true, "en12": false]
    private var reports: [String: [ConnectivityReport]] = [:]
    private var proofs: [String: CampusDiscovery] = [:]
    private var replies: [String: [Reply]] = [:]
    private var awaitingConfirmation: Set<String> = []
    private var gates: Set<String> = []
    private var waiting: [String: CheckedContinuation<Void, Never>] = [:]

    private func key(_ stage: Stage, _ interface: String) -> String { stage.rawValue + "#" + interface }
    func pauseNext(_ stage: Stage, on interface: String) { gates.insert(key(stage, interface)) }
    func isWaiting(_ stage: Stage, on interface: String) -> Bool { waiting[key(stage, interface)] != nil }
    func release(_ stage: Stage, on interface: String) {
        guard let pending = waiting.removeValue(forKey: key(stage, interface)) else { fatalError("No suspended fake request") }
        pending.resume()
    }
    private func suspendIfRequested(_ stage: Stage, on interface: String) async {
        let value = key(stage, interface)
        guard gates.remove(value) != nil else { return }
        // Deliberately ignore cancellation until released: the real model must
        // reject obsolete replies using operation ownership and physical context.
        await withCheckedContinuation { waiting[value] = $0 }
    }
    func configure(_ interface: String, online: Bool? = nil, reports: [ConnectivityReport]? = nil,
                   proof: CampusDiscovery? = nil, replies: [Reply]? = nil) {
        if let online { self.online[interface] = online }
        if let reports { self.reports[interface] = reports }
        if let proof { proofs[interface] = proof }
        if let replies { self.replies[interface] = replies }
    }
    func removePhysicalInterface(_ interface: String) { availableInterfaces.remove(interface) }
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) -> Bool {
        binding = route
        bindings.append(route?.interfaceName)
        return true
    }
    func resetConnections() { binding = nil }
    private func currentInterface() -> String {
        guard let name = binding?.interfaceName, name == "en0" || name == "en12" else {
            fatalError("A dual-attachment request must have an admitted fake interface")
        }
        return name
    }
    func probe() async -> ConnectivityReport {
        // Capture immutable request inputs before suspension or another binding.
        let name = currentInterface()
        requests.append(Request(stage: .probe, interface: name, area: nil, submitted: false, route: binding))
        let report: ConnectivityReport
        if var queue = reports[name], !queue.isEmpty {
            report = queue.removeFirst(); reports[name] = queue
        } else { report = ConnectivityReport(apple: online[name] == true, microsoft: online[name] == true) }
        await suspendIfRequested(awaitingConfirmation.contains(name) ? .confirmation : .probe, on: name)
        return report
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        let name = currentInterface()
        requests.append(Request(stage: .discovery, interface: name, area: area, submitted: false, route: binding))
        let proof = proofs[name] ?? CampusDiscovery(teaching: .confirmed,
            dormitory: name == "en12" ? .confirmed : .notChecked, teachingClientBound: name == "en12")
        await suspendIfRequested(.discovery, on: name)
        return proof
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        await attemptLogin(credentials: credentials, area: area, requireBoundClientConfiguration: false)
    }
    func attemptLogin(credentials: Credentials, area: CampusArea, requireBoundClientConfiguration: Bool) async -> LoginAttempt {
        guard credentials.account == "dual-attachment-fixture-account",
              credentials.password == "dual-attachment-fixture-password", area != .automatic else {
            fatalError("Only explicit-area fixture credentials may be used")
        }
        let name = currentInterface()
        let expectedRoute = binding
        requests.append(Request(stage: .preflight, interface: name, area: area, submitted: false, route: expectedRoute))
        await suspendIfRequested(.preflight, on: name)
        guard !Task.isCancelled, binding == expectedRoute, availableInterfaces.contains(name) else {
            return LoginAttempt(outcome: .cancelled)
        }
        if requireBoundClientConfiguration {
            let proof = proofs[name] ?? CampusDiscovery(teaching: .confirmed,
                dormitory: name == "en12" ? .confirmed : .notChecked, teachingClientBound: name == "en12")
            guard proof.teachingConfirmed && proof.teachingClientBound else {
                return LoginAttempt(error: PortalError.protocolConflict)
            }
        }
        var queue = replies[name] ?? []
        guard !queue.isEmpty else { fatalError("Unexpected fake authentication attempt") }
        let reply = queue.removeFirst(); replies[name] = queue
        requests.append(Request(stage: .attempt, interface: name, area: area, submitted: reply.submitted,
                                requiresBoundClient: requireBoundClientConfiguration, route: expectedRoute))
        await suspendIfRequested(.attempt, on: name)
        if reply.attempt.outcome == .success { online[name] = true; awaitingConfirmation.insert(name) }
        return reply.attempt
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        fatalError("Dual attachment recovery must use fresh staged discovery")
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        fatalError("Dual attachment recovery must use classified single-area attempts")
    }
    func snapshot() -> Snapshot { Snapshot(bindings: bindings, requests: requests) }
}

@MainActor private final class DualAttachmentClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct DualAttachmentRecoveryAppModelTests {
    private static var assertions = 0
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let wifi = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:93:19:01", "gwip": "192.0.2.1", "net": "192.0.2.0/24",
        "dhcp": "192.0.2.2", "dns": "192.0.2.53"
    ])
    private static let ethernet = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:93:19:02", "gwip": "198.51.100.1", "net": "198.51.100.0/24",
        "dhcp": "198.51.100.2", "dns": "198.51.100.53"
    ])
    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        assertions += 1; print("PASS: \(description)")
    }
    private static func folder() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-dual-attachment-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        return root
    }
    private static func seed(_ root: URL, rememberEthernet: Bool = true, rememberWiFi: Bool = true,
                             ethernetArea: String = "dormitory", ethernetDormitoryProof: Bool = true) throws {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        if rememberWiFi {
            guard memory.remember(wifi, area: "teaching", proofTeaching: true, proofDormitory: false,
                now: date.addingTimeInterval(-3_600), newID: "a193abcd") != nil else { fatalError("Invalid Wi-Fi fixture") }
        }
        if rememberEthernet {
            guard memory.remember(ethernet, area: ethernetArea, proofTeaching: true, proofDormitory: ethernetDormitoryProof,
                now: date.addingTimeInterval(-3_600), newID: "b194abcd") != nil else { fatalError("Invalid Ethernet fixture") }
        }
        try NetworkMemoryFile.write(memory.encoded(), to: root.appendingPathComponent("network-memory.json"))
    }
    private static func route(_ wired: Bool, primary: Bool) -> CampusPhysicalRoute {
        CampusPhysicalRoute(interfaceName: wired ? "en12" : "en0", interfaceIndex: wired ? 12 : 1,
            addresses: [wired ? "198.51.100.12" : "192.0.2.11"],
            dnsServers: [wired ? "198.51.100.53" : "192.0.2.53"],
            identity: wired ? "fake-ethernet-lease" : "fake-wifi-lease",
            serviceID: wired ? "fake-ethernet-service" : "fake-wifi-service",
            descriptor: wired ? ethernet : wifi, isSystemPrimary: primary)
    }
    private static func observe(_ model: AppModel, ethernetAttached: Bool, reverse: Bool = false,
                                tunnel: Bool = false, fingerprint: String = "fake-dual-topology") {
        var candidates = ethernetAttached
            ? [route(false, primary: false), route(true, primary: true)] : [route(false, primary: true)]
        if reverse { candidates.reverse() }
        observe(model, candidates: candidates, tunnel: tunnel, fingerprint: fingerprint)
    }
    private static func observe(_ model: AppModel, candidates: [CampusPhysicalRoute],
                                tunnel: Bool = false, fingerprint: String = "fake-dual-topology") {
        var continuity = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: fingerprint), linkActive: true)
        continuity.tunnel = tunnel
        continuity.physicalLabels = candidates.map { ($0.descriptor?.kind == "wifi" ? "Wi-Fi " : "有线 ") + $0.interfaceName }
        continuity.systemPhysicalLabel = candidates.first(where: \.isSystemPrimary).map {
            ($0.descriptor?.kind == "wifi" ? "Wi-Fi " : "有线 ") + $0.interfaceName
        }
        continuity.connectedPhysicalLabels = continuity.physicalLabels
        continuity.hasUsablePhysicalLink = true
        continuity.requiresPhysicalBinding = true
        continuity.sessionOnlyTrust = true
        model.applyEnvironmentObservation(PathObservation(usable: true, interfaces: "fake independent attachments",
            constrained: false, continuity: continuity, physicalCandidates: candidates))
    }
    private static func fixture(_ root: URL, service: DualAttachmentService, clock: DualAttachmentClock,
                                readOnly: Bool = false) -> AppModel {
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: { clock.date },
            recoveryStore: root.appendingPathComponent("recovery.json"),
            isolatedCredentials: Credentials(account: "dual-attachment-fixture-account", password: "dual-attachment-fixture-password"),
            memoryStore: root.appendingPathComponent("network-memory.json"))
        model.area = .automatic
        model.setProbeMode(.complete)
        model.setMonitoringMode(.recover)
        return model
    }
    private static func rounds(_ model: AppModel, count: Int = 2) async {
        for _ in 0..<count { await model.runAutomaticCycle() }
    }
    private static func waitUntil(_ description: String, _ predicate: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await predicate() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(description)")
    }

    static func main() async throws {
        setbuf(stdout, nil)
        // The already-online Wi-Fi attachment remains connected throughout. Only
        // its newly preferred, independently remembered Ethernet peer is offline.
        for reverse in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: false)
            await model.runAutomaticCycle()
            check(model.isOnline && !model.usingWiredCampusRoute, "\(reverse): Wi-Fi is initially online on its own binding")
            let initial = await service.snapshot()
            check(initial.requests.filter { $0.stage == .discovery }.map(\.interface) == ["en0"]
                  && initial.attempts.isEmpty,
                  "\(reverse): the initial online Wi-Fi confirms only its own remembered campus service without authentication")
            await service.configure("en12", replies: [.accepted])
            observe(model, ethernetAttached: true, reverse: reverse)
            check(model.usingWiredCampusRoute && !model.isOnline && model.campusVerificationState == "notChecked",
                  "\(reverse): a remembered primary Ethernet attachment starts without Wi-Fi connectivity or campus proof")
            await rounds(model)
            let snapshot = await service.snapshot()
            check(snapshot.submissions.count == 1 && snapshot.submissions.first?.interface == "en12"
                  && snapshot.submissions.first?.area == .teaching
                  && snapshot.submissions.first?.requiresBoundClient == true && model.isOnline,
                  "\(reverse): fresh client-bound HTTPS evidence recovers Ethernet with one SRun attempt and no HTTP redirect hint")
            check(snapshot.requests.filter { $0.stage == .discovery }.map(\.interface) == ["en0", "en12"],
                  "\(reverse): Ethernet authentication has its own same-interface fresh discovery")
            observe(model, ethernetAttached: false)
            check(!model.usingWiredCampusRoute && !model.isOnline,
                  "\(reverse): removing Ethernet returns to Wi-Fi without borrowing Ethernet online state")
            await model.runAutomaticCycle()
            let returnedToWiFi = await service.snapshot()
            check(model.isOnline && returnedToWiFi.submissions.count == 1,
                  "\(reverse): still-online Wi-Fi needs no extra authentication after Ethernet removal")
            check(returnedToWiFi.requests.filter { $0.stage == .discovery }.map(\.interface) == ["en0", "en12", "en0"],
                  "\(reverse): returning to Wi-Fi obtains fresh same-interface service proof instead of reusing either attachment's old proof")
        }

        // An obsolete Wi-Fi read may return after the preferred attachment changed.
        // No second artificial path notification is used to make the handoff work.
        for stage in [DualAttachmentService.Stage.probe, .discovery] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: false)
            if stage == .discovery {
                await service.configure("en0", online: false)
                await model.runAutomaticCycle()
            }
            await service.pauseNext(stage, on: "en0")
            let old = Task { await model.runAutomaticCycle() }
            try await waitUntil("old Wi-Fi \(stage)") { await service.isWaiting(stage, on: "en0") }
            await service.configure("en12", replies: [.accepted])
            observe(model, ethernetAttached: true)
            check(model.usingWiredCampusRoute && !model.isOnline,
                  "\(stage): a read-only Wi-Fi operation does not pin a newly preferred remembered Ethernet attachment")
            await service.release(stage, on: "en0"); await old.value
            let afterOldReply = await service.snapshot()
            check(!model.isOnline && model.campusVerificationState == "notChecked"
                  && afterOldReply.attempts.isEmpty,
                  "\(stage): a late Wi-Fi result cannot publish connectivity, campus proof or credentials for Ethernet")
            await rounds(model)
            let snapshot = await service.snapshot()
            check(snapshot.submissions.map(\.interface) == ["en12"] && model.isOnline,
                  "\(stage): the new Ethernet operation recovers using fresh independent requests")
            check(snapshot.requests.filter { $0.stage == stage }.first?.interface == "en0",
                  "\(stage): immutable fake request snapshots retain their original interface after rebinding")
        }

        // A disappearing Ethernet proof cannot authenticate the surviving Wi-Fi.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            await model.runAutomaticCycle()
            await service.pauseNext(.discovery, on: "en12")
            let old = Task { await model.runAutomaticCycle() }
            try await waitUntil("Ethernet proof before removal") { await service.isWaiting(.discovery, on: "en12") }
            observe(model, ethernetAttached: false)
            await service.release(.discovery, on: "en12"); await old.value
            check(!model.usingWiredCampusRoute && !model.isOnline && model.campusVerificationState == "notChecked",
                  "Removing Ethernet invalidates its pending proof before returning to remembered Wi-Fi")
            await model.runAutomaticCycle()
            let returnedToWiFi = await service.snapshot()
            check(model.isOnline && returnedToWiFi.attempts.isEmpty,
                  "The surviving Wi-Fi is tested independently and never uses the obsolete Ethernet proof")
        }

        for reverse in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, rememberEthernet: false)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: false); await model.runAutomaticCycle()
            let originalMemory = model.rememberedNetworks
            observe(model, ethernetAttached: true, reverse: reverse, tunnel: true)
            check(model.usingWiredCampusRoute && !model.networkRemembered && !model.currentNetworkAuthorized
                  && !model.isOnline && model.campusVerificationState == "notChecked",
                  "\(reverse): an unknown reliable primary Ethernet replaces known Wi-Fi without borrowing online state, proof or eligibility")
            await rounds(model)
            var snapshot = await service.snapshot()
            check(model.usingWiredCampusRoute && snapshot.attempts.isEmpty
                  && snapshot.requests.filter { $0.stage == .probe }.suffix(2).allSatisfy { $0.interface == "en12" }
                  && model.rememberedNetworks == originalMemory,
                  "\(reverse): unknown Ethernet is independently observed with zero credential attempts and no automatic memory mutation")
            check(snapshot.requests.allSatisfy { request in
                let expected = route(request.interface == "en12", primary: false)
                return request.route == expected
            }, "\(reverse): every fake request retains the correct independent source, subnet, gateway, DNS and service identity")
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("explicit unknown primary Ethernet memory") { !model.busy }
            check(model.networkRemembered && model.rememberedNetworks.count == 2
                  && model.rememberedNetworks.contains { $0.id == "a193abcd" }
                  && !model.currentNetworkAuthorized,
                  "\(reverse): explicit verification remembers only Ethernet and retains the original Wi-Fi record without session trust")
            await service.configure("en12", replies: [.accepted])
            await rounds(model)
            snapshot = await service.snapshot()
            check(model.isOnline && snapshot.submissions.map(\.interface) == ["en12"]
                  && snapshot.submissions.first?.requiresBoundClient == true,
                  "\(reverse): only explicitly remembered Ethernet can later recover using its own fresh bound service and one submission")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, rememberEthernet: false)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            await service.configure("en12", proof: CampusDiscovery(teaching: .unconfirmed, dormitory: .unconfirmed))
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: false); await model.runAutomaticCycle()
            observe(model, ethernetAttached: true, tunnel: true)
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("new primary Ethernet failed identity") { !model.busy }
            check(!model.networkRemembered && !model.currentNetworkAuthorized && !model.isOnline
                  && model.campusVerificationState == "unconfirmed" && model.rememberedNetworks.count == 1,
                  "A failed new Ethernet identity cannot use the old Wi-Fi proof or acquire its eligibility")
            observe(model, ethernetAttached: false, tunnel: true)
            check(model.networkRemembered && !model.isOnline && model.campusVerificationState == "notChecked",
                  "Switching back to Wi-Fi rematches its saved record but obtains no online result or proof from Ethernet")
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(model.isOnline && snapshot.requests.last?.interface == "en0" && snapshot.attempts.isEmpty,
                  "Returning Wi-Fi completes its own probe and identity with zero credential submissions")
        }

        for reverse in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            var candidates = [route(false, primary: false), route(true, primary: false)]
            if reverse { candidates.reverse() }
            observe(model, candidates: candidates, tunnel: true)
            check(!model.usingWiredCampusRoute && !model.canVerifyAndRememberNetwork,
                  "\(reverse): two unknown physical candidates without a reliable primary cannot be chosen by Ethernet type or enumeration order")
            candidates = [route(false, primary: true), route(true, primary: true)]
            if reverse { candidates.reverse() }
            observe(model, candidates: candidates, tunnel: true)
            check(!model.usingWiredCampusRoute && !model.canVerifyAndRememberNetwork,
                  "\(reverse): conflicting primary claims block selection instead of accepting the first candidate")
            observe(model, candidates: [route(true, primary: false)], tunnel: true)
            check(model.usingWiredCampusRoute && model.canVerifyAndRememberNetwork && !model.networkRemembered,
                  "\(reverse): one independently valid sole candidate remains observable without inventing memory or requiring a primary flag")
        }

        for reverse in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, rememberEthernet: false)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            var candidates = [route(true, primary: false), route(false, primary: false)]
            if reverse { candidates.reverse() }
            observe(model, candidates: candidates, tunnel: true)
            check(!model.usingWiredCampusRoute && model.networkRemembered && !model.currentNetworkAuthorized,
                  "\(reverse): absent a reliable primary, a known Wi-Fi fallback is not displaced merely by Ethernet type")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, rememberWiFi: false)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, candidates: [route(true, primary: true)], tunnel: true)
            check(model.usingWiredCampusRoute && model.networkRemembered,
                  "A remembered Ethernet fixture initially selects its sole reliable attachment")
            observe(model, candidates: [route(true, primary: false), route(false, primary: true)], tunnel: true)
            check(!model.usingWiredCampusRoute && !model.networkRemembered && !model.currentNetworkAuthorized,
                  "The reliable system-primary policy also prefers an unknown Wi-Fi over a remembered secondary Ethernet")
        }

        // An explicitly trusted session can recover without a persistent record.
        // Once preflight begins, the entire credential operation remains bound to it,
        // even when a remembered Ethernet peer becomes the new system primary.
        for stage in [DualAttachmentService.Stage.preflight, .attempt, .confirmation] {
            for disappears in [false, true] {
                let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
                try seed(root, rememberWiFi: false)
                let service = DualAttachmentService(), clock = DualAttachmentClock()
                await service.configure("en0", online: false,
                    proof: CampusDiscovery(teaching: .confirmed, dormitory: .notChecked, teachingClientBound: true),
                    replies: [.accepted])
                let model = fixture(root, service: service, clock: clock)
                observe(model, ethernetAttached: false, tunnel: true)
                model.verifyCurrentCampusService()
                try await waitUntil("temporary original Wi-Fi service trust") { !model.busy }
                model.authorizeCurrentNetwork()
                check(!model.networkRemembered && model.currentNetworkAuthorized,
                      "\(stage)/\(disappears): explicit session trust permits recovery while the original Wi-Fi remains outside known records")
                await model.runAutomaticCycle()
                await service.pauseNext(stage, on: "en0")
                let old = Task { await model.runAutomaticCycle() }
                try await waitUntil("unknown original route \(stage)") { await service.isWaiting(stage, on: "en0") }
                if disappears { await service.removePhysicalInterface("en0") }
                observe(model, candidates: disappears
                    ? [route(true, primary: true)]
                    : [route(true, primary: true), route(false, primary: false)], tunnel: true)
                if disappears {
                    check(model.usingWiredCampusRoute && !model.busy && !model.isOnline,
                          "\(stage): disappearing original route cancels ownership before adopting the new primary")
                } else {
                    check(!model.usingWiredCampusRoute && model.busy && !model.networkRemembered,
                          "\(stage): an active original candidate remains fixed even outside the known set")
                }
                await service.release(stage, on: "en0")
                await old.value
                let snapshot = await service.snapshot()
                check(snapshot.submissions.count == (stage == .preflight && disappears ? 0 : 1)
                      && snapshot.submissions.allSatisfy { $0.interface == "en0" && $0.route == route(false, primary: false) }
                      && snapshot.requests.allSatisfy { $0.interface == "en0" },
                      "\(stage)/\(disappears): preflight, credentials and confirmation never move to the preferred Ethernet or duplicate a submission")
                if disappears {
                    check(!model.isOnline && model.campusVerificationState == "notChecked",
                          "\(stage): late original-route success cannot restore connectivity or proof on Ethernet")
                    if stage == .attempt {
                        check(model.holds.contains { $0.reason == .uncertain },
                              "An interrupted submitted request remains result-unknown on the new primary")
                    }
                } else {
                    check(model.isOnline && !model.usingWiredCampusRoute,
                          "\(stage): confirmation finishes successfully on the retained original physical route")
                    observe(model, ethernetAttached: true, tunnel: true)
                    check(model.usingWiredCampusRoute && !model.isOnline && model.campusVerificationState == "notChecked",
                          "\(stage): only after completion can a fresh observation adopt Ethernet without the Wi-Fi result or proof")
                }
            }
        }

        // Multiple active physical links require fresh HTTPS identity even without TUN.
        for clientBound in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            await service.configure("en12", proof: CampusDiscovery(teaching: .unconfirmed, dormitory: .confirmed,
                                                                   teachingClientBound: clientBound))
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true, tunnel: false)
            await rounds(model)
            check((await service.snapshot()).attempts.isEmpty && !model.currentNetworkAuthorized,
                  "Missing HTTPS identity blocks both protocols with two links even when clientBound=\(clientBound)")
        }

        // A new network's read-only setup must remember the verified service,
        // rather than labelling every dual-service HTTPS page as dormitory.
        for variant in ["automatic", "explicitDormitory", "missingTLS"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            await service.configure("en12", proof: CampusDiscovery(
                teaching: variant == "missingTLS" ? .unconfirmed : .confirmed,
                dormitory: .confirmed, teachingClientBound: true))
            let model = fixture(root, service: service, clock: clock)
            if variant == "explicitDormitory" { model.area = .dormitory }
            observe(model, ethernetAttached: true)
            check(model.usingWiredCampusRoute && model.canVerifyAndRememberNetwork && model.rememberedNetworks.isEmpty,
                  "\(variant): a new primary Ethernet attachment can start credential-free network verification")
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("new Ethernet memory verification") { !model.busy }
            let snapshot = await service.snapshot()
            check(snapshot.requests.filter { $0.stage == .discovery }.map(\.interface) == ["en12"]
                  && snapshot.attempts.isEmpty && snapshot.submissions.isEmpty,
                  "\(variant): remembering a new Ethernet attachment only discovers its own services and never logs in")
            if variant == "missingTLS" {
                check(!model.networkRemembered && model.rememberedNetworks.isEmpty
                      && !FileManager.default.fileExists(atPath: root.appendingPathComponent("network-memory.json").path),
                      "An unconfirmed HTTPS identity cannot create a memory even with dormitory and client-bound flags")
            } else {
                let dormitory = variant == "explicitDormitory"
                let expectedArea = dormitory ? "dormitory" : "teaching"
                let record = model.rememberedNetworks.first
                check(model.networkRemembered && model.rememberedNetworks.count == 1
                      && record?.area == expectedArea && record?.proofTeaching == true
                      && record?.proofDormitory == dormitory,
                      "\(variant): fresh client-bound scope is remembered as \(expectedArea), preserving an explicit area choice")
                let stored = NetworkMemory.decode(try Data(contentsOf: root.appendingPathComponent("network-memory.json")), now: date)
                check(stored?.count == 1 && stored?.records.first?.area == expectedArea
                      && stored?.records.first?.proofDormitory == dormitory,
                      "\(variant): the remembered service scope is committed consistently to isolated storage")
            }
        }

        // UI actions and readiness share the actual candidate's eligibility, not
        // merely the presence of a record or reachability of the other service.
        for variant in ["teachingConfig", "dormitoryMemoryTeachingConfig", "missingTLS", "ambiguous"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, ethernetArea: variant == "teachingConfig" ? "teaching" : "dormitory",
                     ethernetDormitoryProof: variant != "teachingConfig")
            let store = root.appendingPathComponent("network-memory.json")
            let original = try Data(contentsOf: store)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            await service.configure("en12", proof: CampusDiscovery(
                teaching: variant == "missingTLS" ? .unconfirmed : .confirmed, dormitory: .confirmed,
                teachingClientBound: variant == "teachingConfig" || variant == "dormitoryMemoryTeachingConfig"))
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            model.verifyCurrentCampusService()
            try await waitUntil("remembered candidate service-only verification") { !model.busy }
            let snapshot = await service.snapshot()
            let preservedBytes = try Data(contentsOf: store)
            check(model.networkRemembered && !model.currentNetworkAuthorized
                  && snapshot.attempts.isEmpty && snapshot.submissions.isEmpty && preservedBytes == original,
                  "\(variant): service-only verification preserves the old record without granting trust or submitting credentials")
            check(!model.requiresNetworkConfirmation && !model.needsNetworkMemoryUpdate && !model.shouldOfferRememberNetwork,
                  "\(variant): record and candidate facts do not invent a missing-eligibility update action")
            if variant == "teachingConfig" || variant == "dormitoryMemoryTeachingConfig" {
                check(model.currentCampusRouting == .selected(.teaching, allowUnsentFallback: false)
                      && !model.needsDormitoryRecoveryEligibility && model.autoLoginKey == "ready"
                      && model.readiness.title == "ready.title.ready" && model.recoverySetupHasNetwork,
                      "\(variant): fresh bound teaching evidence is ready without borrowing dormitory trust or showing a false trust warning")
            } else {
                check(model.autoLoginKey == "awaitingProof" && model.presentation.banner?.key != "banner.needsTrust",
                      "\(variant): missing or ambiguous campus evidence requests proof or selection, never unrelated session trust")
                if variant == "ambiguous" {
                    check(model.currentCampusRouting == .needsSelection && model.entrySelectionNeeded,
                          "Two reachable services without a concrete candidate keep the area-selection action available")
                }
            }
        }

        for variant in ["explicitDormitory", "dormitoryHint", "pausedDormitoryHint"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, ethernetArea: "teaching", ethernetDormitoryProof: false)
            let store = root.appendingPathComponent("network-memory.json"), original = try Data(contentsOf: store)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            if variant == "explicitDormitory" { model.area = .dormitory }
            observe(model, ethernetAttached: true)
            if variant != "explicitDormitory" {
                let currentDormitoryHint = ConnectivityReport(
                    appleResult: .init(state: .network, elapsed: 0),
                    microsoftResult: .init(state: .redirect, elapsed: 0, campusAreaHint: .dormitory), mode: .complete)
                // The displayed check, explicit service check and memory update each need
                // their own current hint. A historical redirect must not supply all three.
                await service.configure("en12", reports: Array(repeating: currentDormitoryHint, count: 3))
                model.checkConnection()
                try await waitUntil("fresh bound dormitory probe hint") { !model.busy }
            }
            if variant == "pausedDormitoryHint" { model.pauseAutomaticRecovery() }
            model.verifyCurrentCampusService()
            try await waitUntil("insufficient remembered scope service-only verification") { !model.busy }
            let afterVerification = await service.snapshot(), expectedAuto = variant == "pausedDormitoryHint" ? "paused" : "needsTrust"
            let verifiedBytes = try Data(contentsOf: store)
            check(model.networkRemembered && model.dormitoryMemoryLevel == .none && !model.currentNetworkAuthorized
                  && model.needsDormitoryRecoveryEligibility && model.requiresNetworkConfirmation
                  && model.needsNetworkMemoryUpdate && model.shouldOfferRememberNetwork
                  && model.canVerifyAndRememberNetwork && !model.recoverySetupHasNetwork && model.autoLoginKey == expectedAuto,
                  "\(variant): insufficient remembered scope exposes an enabled update action instead of only verify and forget")
            check(afterVerification.attempts.isEmpty && afterVerification.submissions.isEmpty && verifiedBytes == original,
                  "\(variant): fresh service evidence alone does not upgrade the stored permission or submit credentials")
            if variant == "pausedDormitoryHint" {
                check(model.pausedForAutomatic && model.readiness.title == "ready.title.paused",
                      "A missing memory scope cannot hide or release an explicit user pause")
            }
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized && !model.requiresNetworkConfirmation
                  && model.needsNetworkMemoryUpdate && model.shouldOfferRememberNetwork && !model.recoverySetupHasNetwork,
                  "\(variant): session trust leaves the permanent memory-update action reachable")
            check(!model.readinessInput.remembered
                  && model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.trusted"
                  && !model.recoverySetupHasNetwork && model.shouldOfferRememberNetwork,
                  "\(variant): a temporary trust chip cannot imply persistent eligibility while the memory update is incomplete")
            model.verifyAndRememberCurrentNetwork()
            try await waitUntil("explicit remembered scope update") { !model.busy }
            let updated = NetworkMemory.decode(try Data(contentsOf: store), now: date)
            let ethernetRecord = updated?.records.first { $0.id == "b194abcd" }
            let afterUpdate = await service.snapshot()
            check(ethernetRecord?.area == "dormitory" && ethernetRecord?.proofTeaching == true
                  && ethernetRecord?.proofDormitory == true && updated?.count == 2
                  && !model.needsNetworkMemoryUpdate && !model.shouldOfferRememberNetwork && model.recoverySetupHasNetwork,
                  "\(variant): an explicit update commits only the required proven service scope and restores normal verify-only actions")
            check(model.readinessInput.remembered
                  && model.readiness.items.first(where: { $0.kind == "network" })?.key == "ready.network.remembered"
                  && model.recoverySetupHasNetwork && !model.shouldOfferRememberNetwork,
                  "\(variant): the remembered chip returns only after persistent eligibility has actually been updated")
            check(afterUpdate.attempts.isEmpty && afterUpdate.submissions.isEmpty,
                  "\(variant): clicking session trust and remember updates eligibility without logging in")
            if variant == "pausedDormitoryHint" {
                check(model.pausedForAutomatic && model.autoLoginKey == "paused"
                      && model.holds.contains { $0.reason == .userPaused },
                      "Completing a memory update leaves the original user pause intact")
            }
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let store = root.appendingPathComponent("network-memory.json"), original = try Data(contentsOf: store)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let currentDormitoryHint = ConnectivityReport(
                appleResult: .init(state: .network, elapsed: 0),
                microsoftResult: .init(state: .redirect, elapsed: 0, campusAreaHint: .dormitory), mode: .complete)
            // The explicit service operation obtains a second current physical hint;
            // it must not borrow the displayed check's historical redirect.
            await service.configure("en12", reports: [currentDormitoryHint, currentDormitoryHint],
                proof: CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed, teachingClientBound: true))
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            model.checkConnection()
            try await waitUntil("new dormitory hint before service scope failure") { !model.busy }
            let visibleReport = model.lastInternetCheck?.base.summary, checkedAt = model.lastCheckedAt
            model.verifyCurrentCampusService()
            try await waitUntil("unconfirmed required dormitory service") { !model.busy }
            let snapshot = await service.snapshot(), preservedBytes = try Data(contentsOf: store)
            check(model.currentCampusRouting == .unconfirmed && model.needsDormitoryRecoveryEligibility
                  && model.campusVerificationState == "confirmed" && model.requiredCampusVerificationState == "unconfirmed"
                  && model.campusVerificationDisplayState == "unconfirmed" && model.campusVerificationHintKey == "campus.verify.failedHint",
                  "A fresh dormitory hint keeps its required service unconfirmed even when independent teaching identity passes")
            check(!model.currentNetworkAuthorized && model.autoLoginKey == "awaitingProof"
                  && snapshot.attempts.isEmpty && snapshot.submissions.isEmpty && preservedBytes == original,
                  "An unavailable hinted dormitory service cannot use teaching proof to submit credentials or upgrade memory")
            check(snapshot.requests.filter { $0.stage == .probe && $0.interface == "en12" }.count == 2
                  && model.lastInternetCheck?.base.summary == visibleReport && model.lastCheckedAt == checkedAt,
                  "Required dormitory failure uses its own fresh bound hint while preserving the earlier visible report")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, ethernetArea: "teaching", ethernetDormitoryProof: false)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            await service.configure("en12", proof: CampusDiscovery(teaching: .confirmed, dormitory: .confirmed))
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            model.connect()
            try await waitUntil("manual dual-service selection") { !model.busy }
            let snapshot = await service.snapshot()
            check(model.currentCampusRouting == .needsSelection && model.entrySelectionNeeded
                  && model.autoLoginKey == "awaitingProof" && model.presentation.primary == "area",
                  "A manual round's actual ambiguous candidate retains area selection despite an older teaching preference")
            check(!model.currentNetworkAuthorized && !model.requiresNetworkConfirmation
                  && !model.needsNetworkMemoryUpdate && !model.shouldOfferRememberNetwork && snapshot.attempts.isEmpty,
                  "Manual ambiguity does not invent trust requirements or use old teaching memory to authorize a login")
            await service.configure("en12", online: true)
            model.checkConnection()
            try await waitUntil("complete public connectivity after area ambiguity") { !model.busy }
            let recovered = await service.snapshot()
            check(model.isOnline && model.lastInternetCheck?.base.allPassed == true
                  && !model.entrySelectionNeeded && model.autoLoginKey == "ready" && model.presentation.primary != "area",
                  "A later complete connectivity success removes stale area-selection prompts without another login")
            check(!model.currentNetworkAuthorized && recovered.attempts.isEmpty && model.networkRemembered,
                  "Clearing a stale area-selection prompt is presentation only and grants no session authorization")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root, ethernetArea: "teaching", ethernetDormitoryProof: false)
            let store = root.appendingPathComponent("network-memory.json"), original = try Data(contentsOf: store)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock, readOnly: true)
            model.area = .dormitory
            observe(model, ethernetAttached: true)
            check(!model.shouldOfferRememberNetwork && !model.canVerifyAndRememberNetwork && !model.canVerifyCurrentCampusService,
                  "A read-only launch hides all memory-writing and service-verification actions")
            model.verifyAndRememberCurrentNetwork(); model.authorizeCurrentNetwork()
            let snapshot = await service.snapshot()
            let preservedBytes = try Data(contentsOf: store)
            check(!model.currentNetworkAuthorized && snapshot.requests.isEmpty && preservedBytes == original,
                  "Read-only update and trust calls neither mutate eligibility nor send fake network requests")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let store = root.appendingPathComponent("network-memory.json"), original = try Data(contentsOf: store)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            model.pauseAutomaticRecovery()
            let holds = model.holds, records = model.rememberedNetworks
            model.verifyCurrentCampusService()
            try await waitUntil("service feedback before changing area") { !model.busy }
            check(model.memoryVerificationMessage == SharedCopy.t("campus.verify.confirmed"),
                  "Successful service-only verification exposes feedback for its checked area")
            model.area = .dormitory
            check(model.memoryVerificationMessage == nil,
                  "Changing the selected area immediately revokes earlier service-only feedback")
            model.area = .automatic
            let snapshot = await service.snapshot(), preservedBytes = try Data(contentsOf: store)
            check(model.memoryVerificationMessage == nil && model.holds == holds && model.rememberedNetworks == records
                  && preservedBytes == original && snapshot.attempts.isEmpty && snapshot.submissions.isEmpty,
                  "Returning to an earlier area cannot resurrect stale feedback or change holds, memory or credentials")
        }

        for feedback in ["serviceSuccess", "memoryWriteFailure"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let store = root.appendingPathComponent("network-memory.json"), original = try Data(contentsOf: store)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            if feedback == "serviceSuccess" {
                model.verifyCurrentCampusService()
            } else {
                // An existing directory is never a valid atomic-write destination.
                // Keep the original fixture file intact at a private backup path.
                let backup = root.appendingPathComponent("original-memory-fixture.json")
                try FileManager.default.moveItem(at: store, to: backup)
                try FileManager.default.createDirectory(at: store, withIntermediateDirectories: false)
                model.verifyAndRememberCurrentNetwork()
                try await waitUntil("deterministic isolated memory write failure") { !model.busy }
                try FileManager.default.removeItem(at: store)
                try FileManager.default.moveItem(at: backup, to: store)
            }
            try await waitUntil("verification feedback before new discovery") { !model.busy }
            let expected = SharedCopy.t(feedback == "serviceSuccess" ? "campus.verify.confirmed" : "memory.verification.writeFailed")
            check(model.memoryVerificationMessage == expected,
                  "\(feedback): the isolated fixture starts with the expected specific verification feedback")
            let records = model.rememberedNetworks
            await service.configure("en12", online: false,
                                    proof: CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked))
            await model.runAutomaticCycle()
            await service.pauseNext(.discovery, on: "en12")
            let next = Task { await model.runAutomaticCycle() }
            try await waitUntil("fresh automatic discovery replaces older service feedback") { await service.isWaiting(.discovery, on: "en12") }
            check(model.memoryVerificationMessage == (feedback == "serviceSuccess" ? nil : expected),
                  "\(feedback): starting a fresh discovery clears only stale service-only feedback, preserving a save failure")
            await service.release(.discovery, on: "en12"); await next.value
            let snapshot = await service.snapshot(), preservedBytes = try Data(contentsOf: store)
            check(model.campusVerificationState == "unconfirmed"
                  && model.memoryVerificationMessage == (feedback == "serviceSuccess" ? nil : expected)
                  && snapshot.attempts.isEmpty && snapshot.submissions.isEmpty,
                  "\(feedback): failed fresh campus identity cannot retain a stale success or submit credentials")
            check(model.rememberedNetworks == records && preservedBytes == original,
                  "\(feedback): replacing old feedback never deletes or rewrites the isolated saved network records")
        }

        // A formal request that may already have left the old interface remains owned;
        // changing primary cannot cancel it to obtain another submission allowance.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            await service.configure("en0", online: false, replies: [
                .init(attempt: LoginAttempt(outcome: .uncertain), submitted: true)
            ])
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: false)
            await model.runAutomaticCycle()
            await service.pauseNext(.attempt, on: "en0")
            let old = Task { await model.runAutomaticCycle() }
            try await waitUntil("owned Wi-Fi submission") { await service.isWaiting(.attempt, on: "en0") }
            observe(model, ethernetAttached: true)
            check(!model.usingWiredCampusRoute && model.busy,
                  "A submitted Wi-Fi operation keeps its exact binding until its result is owned")
            check((await service.snapshot()).submissions.map(\.interface) == ["en0"],
                  "Primary changes cannot start a second formal request while the first is in flight")
            await service.release(.attempt, on: "en0"); await old.value
            observe(model, ethernetAttached: true, fingerprint: "fresh-local-observation-after-result")
            clock.advance(300)
            await rounds(model)
            check((await service.snapshot()).submissions.map(\.interface) == ["en0"]
                  && model.holds.contains { $0.reason == .uncertain },
                  "Fresh Ethernet TLS evidence and time cannot bypass a global result-unknown hold")
        }

        for variant in ["userPaused", "credentials", "security", "uncertain", "cooldown"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            try seed(root)
            let reason: HoldReason? = variant == "userPaused" ? .userPaused : variant == "credentials" ? .credentials
                : variant == "security" ? .security : variant == "uncertain" ? .uncertain : nil
            let holds = reason.map { [AuthenticationHold(reason: $0, generation: $0 == .credentials ? 1 : nil,
                origin: $0 == .userPaused ? .user : .global, rejection: $0 == .credentials ? "password" : "")] } ?? []
            try RecoveryJournal(generation: 1, holds: holds,
                notBefore: variant == "cooldown" ? date.addingTimeInterval(90) : nil,
                pendingSubmission: variant == "uncertain").write(root.appendingPathComponent("recovery.json"))
            let service = DualAttachmentService(), clock = DualAttachmentClock()
            let model = fixture(root, service: service, clock: clock)
            observe(model, ethernetAttached: true)
            await rounds(model)
            check((await service.snapshot()).attempts.isEmpty,
                  "\(variant): client-bound HTTPS discovery cannot override an existing global hold or cooldown")
            if reason != nil { check(model.holds == holds, "\(variant): the original protective hold is retained") }
        }
        print("Dual attachment recovery AppModel tests: \(assertions) assertions passed; per-interface fake services only")
    }
}
