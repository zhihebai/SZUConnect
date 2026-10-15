import Foundation

private final class RefusingSystemProtocol: URLProtocol, @unchecked Sendable {
    private static let lock = NSLock()
    private static var count = 0
    static var hits: Int { lock.withLock { count } }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.lock.withLock { Self.count += 1 }
        client?.urlProtocol(self, didFailWithError: URLError(.dnsLookupFailed))
    }
    override func stopLoading() {}
}

private final class SelectionBox: @unchecked Sendable {
    private let lock = NSLock()
    private var selection: CampusPhysicalSelection
    init(_ value: CampusPhysicalSelection) { selection = value }
    func get() -> CampusPhysicalSelection { lock.withLock { selection } }
    func set(_ value: CampusPhysicalSelection) { lock.withLock { selection = value } }
}

private final class RouteRecordBox: @unchecked Sendable {
    private let lock = NSLock()
    private var names: [String] = []
    func record(_ name: String) { lock.withLock { names.append(name) } }
    func snapshot() -> [String] { lock.withLock { names } }
}

private actor FixturePhysicalSession: PortalPhysicalSession {
    var paths: [String] = []
    var cancels = 0
    var failure: CampusPhysicalTransportError?
    var status = 200
    var delay: Duration?
    var oversized = false
    var afterRequest: (@Sendable (String) -> Void)?
    func configure(failure: CampusPhysicalTransportError? = nil, status: Int = 200,
                   delay: Duration? = nil, oversized: Bool = false,
                   afterRequest: (@Sendable (String) -> Void)? = nil) {
        self.failure = failure; self.status = status; self.delay = delay; self.oversized = oversized
        self.afterRequest = afterRequest
    }
    func data(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
        let url = request.url!
        paths.append(url.path)
        if let delay { try await Task.sleep(for: delay) }
        if let failure { throw failure }
        let body: String
        switch url.path {
        case "/srun_portal_pc": body = "<html>深圳大学 srun login</html>"
        case "/eportal": body = "<title>ePortal</title>"
        case "/eportal/portal/login": body = #"{"result":0,"msg":"登录成功"}"#
        case "/library/test/success.html": body = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
        case "/connecttest.txt": body = "Microsoft Connect Test"
        default: body = "unexpected fixture path"
        }
        let data = oversized ? Data(repeating: 65, count: limit + 1) : Data(body.utf8)
        afterRequest?(url.path)
        return (data, HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: nil)!)
    }
    func cancel() { cancels += 1 }
    func snapshot() -> ([String], Int) { (paths, cancels) }
}

@main struct CampusPhysicalRoutingTests {
    static var assertions = 0
    static func check(_ value: Bool, _ message: String) {
        precondition(value, message)
        assertions += 1
        print("PASS: \(message)")
    }
    static func route(_ identity: String) -> CampusPhysicalRoute {
        CampusPhysicalRoute(interfaceName: "fixture-en0", interfaceIndex: 99,
                            addresses: ["192.0.2.10"], dnsServers: ["192.0.2.53"], identity: identity)
    }
    private static func client(_ box: SelectionBox, _ auth: FixturePhysicalSession,
                       _ probe: FixturePhysicalSession) -> PortalClient {
        PortalClient(protocolClasses: [RefusingSystemProtocol.self], probeTimeout: 0.2,
                     authenticationTimeout: 0.2, physicalSelectionProvider: { box.get() },
                     physicalTransportFactory: { _, cookies in cookies ? auth : probe })
    }
    static func main() async throws {
        let box = SelectionBox(.bound(route("route-A")))
        let auth = FixturePhysicalSession(), probe = FixturePhysicalSession()
        let service = client(box, auth, probe)
        let discovery = await service.discoverCampus(area: .automatic)
        check(discovery.teaching == .confirmed && discovery.dormitory == .confirmed,
              "A physical channel verifies campus pages while the system DNS path refuses every request")
        check(RefusingSystemProtocol.hits == 0, "Bound requests never leak into the system or TUN transport")
        check(await auth.snapshot().0.isEmpty, "Read-only discovery does not use authentication cookies")
        let online = await service.probe(mode: .complete)
        check(online.allPassed, "Both fixed connectivity probes use the same physical route")
        _ = try await service.login(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        let submissions = await auth.snapshot().0.filter { $0 == "/eportal/portal/login" }
        check(submissions.count == 1, "The existing dormitory flow still submits exactly once on the physical channel")
        check(RefusingSystemProtocol.hits == 0, "Authentication never retries through the refusing system transport")

        box.set(.bound(route("route-B")))
        let before = await probe.snapshot().0.count
        let changed = await service.discoverCampus(area: .automatic)
        check(changed.teaching != .confirmed, "A changed physical route cannot inherit the old channel's campus proof")
        check(await probe.snapshot().0.count == before, "Route changes stop before the old channel sends another request")
        await service.resetConnections()
        let cancelledAuth = await auth.snapshot().1, cancelledProbe = await probe.snapshot().1
        check(cancelledAuth == 1 && cancelledProbe == 1, "Reset cancels both old physical cookie scopes")
        let renewed = await service.discoverCampus(area: .automatic)
        check(renewed.teaching == .confirmed, "A reset can create a fresh channel for the newly observed route")

        for failure in [CampusPhysicalTransportError.dns, .timeout, .tls, .network, .responseTooLarge] {
            let failureProbe = FixturePhysicalSession(), failureAuth = FixturePhysicalSession()
            await failureProbe.configure(failure: failure)
            let failedService = client(SelectionBox(.bound(route("failure"))), failureAuth, failureProbe)
            let failed = await failedService.discoverCampus(area: .automatic)
            check(failed.teaching != .confirmed && failed.dormitory == .notChecked,
                  "A physical \(failure) error preserves the HTTPS-before-HTTP identity gate")
            check(await failureAuth.snapshot().0.isEmpty, "A failed physical discovery submits zero credentials")
        }
        for (status, oversized) in [(302, false), (200, true)] {
            let invalidProbe = FixturePhysicalSession(), invalidAuth = FixturePhysicalSession()
            await invalidProbe.configure(status: status, oversized: oversized)
            let invalidService = client(SelectionBox(.bound(route("invalid"))), invalidAuth, invalidProbe)
            let invalid = await invalidService.discoverCampus(area: .automatic)
            check(invalid.teaching != .confirmed && invalid.dormitory == .notChecked,
                  "Physical replies still enforce redirects and bounded response size")
        }
        let unavailableAuth = FixturePhysicalSession(), unavailableProbe = FixturePhysicalSession()
        let unavailable = client(SelectionBox(.unavailable), unavailableAuth, unavailableProbe)
        let unknown = await unavailable.discoverCampus(area: .automatic)
        check(unknown.teaching != .confirmed && RefusingSystemProtocol.hits == 0,
              "An ambiguous TUN physical route never falls back to system DNS or proxy")
        let noProbe = await unavailableProbe.snapshot().0.isEmpty, noAuth = await unavailableAuth.snapshot().0.isEmpty
        check(noProbe && noAuth,
              "An ambiguous route creates no physical requests")
        let recoveringBox = SelectionBox(.unavailable)
        let recovering = client(recoveringBox, FixturePhysicalSession(), FixturePhysicalSession())
        recoveringBox.set(.bound(route("recovered-DHCP")))
        let recoveredProbe = await recovering.probe(mode: .complete)
        check(recoveredProbe.allPassed, "A new credential-free probe recovers a transient unavailable lease without manual reset")
        let recoveredDiscovery = await recovering.discoverCampus(area: .automatic)
        check(recoveredDiscovery.teaching == .confirmed, "Discovery uses the fresh lease established by the new probe round")

        // Remembering/admitting A must never permit a fresh probe or reset to substitute B.
        let expectedA = route("expected-A"), expectedB = route("expected-B")
        let expectedBox = SelectionBox(.bound(expectedA))
        let expectedAuth = FixturePhysicalSession(), expectedProbe = FixturePhysicalSession()
        let expected = client(expectedBox, expectedAuth, expectedProbe)
        check(await expected.setExpectedPhysicalRoute(expectedA), "The admitted live A lease is accepted")
        check(await expected.discoverCampus(area: .automatic).teaching == .confirmed, "An admitted A lease can acquire current campus proof")
        expectedBox.set(.bound(expectedB))
        await expected.resetConnections()
        let wrongAfterReset = await expected.probe(mode: .complete)
        check(!wrongAfterReset.appleResult.succeeded && !wrongAfterReset.microsoftResult.succeeded && wrongAfterReset.appleResult.state == .cancelled,
              "Reset and probe cannot replace an expected A lease with live B")
        let wrongDiscovery = await expected.discoverCampus(area: .automatic)
        let wrongAttempt = await expected.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        check(wrongDiscovery.teaching != .confirmed && wrongAttempt.outcome == .cancelled,
              "A-memory/B-live mismatch cancels discovery and authentication")
        check(await expectedAuth.snapshot().0.isEmpty, "An expected lease mismatch submits zero credentials")
        check(!(await expected.setExpectedPhysicalRoute(expectedA)), "A stale admission is rejected instead of falling back to system transport")
        _ = await expected.probe(mode: .complete)
        check(await expectedAuth.snapshot().0.isEmpty, "Rejected admission stays pinned across another probe")
        check(await expected.setExpectedPhysicalRoute(expectedB), "An explicit new caller admission accepts current B")
        check(await expected.discoverCampus(area: .automatic).teaching == .confirmed, "Newly admitted B obtains fresh campus proof")
        let admittedB = await expected.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        check(admittedB.outcome == .success, "Only explicit B admission permits its single credential submission")
        check(await expectedAuth.snapshot().0.filter { $0 == "/eportal/portal/login" }.count == 1,
              "Lease replacement cancels old sessions without duplicating login")

        let afterProbeBox = SelectionBox(.bound(expectedA))
        let afterProbeAuth = FixturePhysicalSession(), afterProbeSession = FixturePhysicalSession()
        let afterProbe = client(afterProbeBox, afterProbeAuth, afterProbeSession)
        check(await afterProbe.setExpectedPhysicalRoute(expectedA), "A is admitted before a clean connectivity probe")
        check(await afterProbe.probe(mode: .complete).allPassed, "A's connectivity probe completes before the network changes")
        afterProbeBox.set(.bound(expectedB))
        _ = await afterProbe.probe(mode: .complete)
        let afterProbeAttempt = await afterProbe.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        let afterProbePaths = await afterProbeAuth.snapshot().0
        check(afterProbeAttempt.outcome == .cancelled && afterProbePaths.isEmpty,
              "A successful earlier probe cannot authorize credentials after A changes to B")

        let afterDiscoveryBox = SelectionBox(.bound(expectedA))
        let afterDiscoveryAuth = FixturePhysicalSession(), afterDiscoveryProbe = FixturePhysicalSession()
        let afterDiscovery = client(afterDiscoveryBox, afterDiscoveryAuth, afterDiscoveryProbe)
        check(await afterDiscovery.setExpectedPhysicalRoute(expectedA), "A is admitted before read-only campus discovery")
        check(await afterDiscovery.discoverCampus(area: .automatic).teaching == .confirmed, "A's read-only campus discovery completes")
        afterDiscoveryBox.set(.bound(expectedB))
        let afterDiscoveryAttempt = await afterDiscovery.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        let afterDiscoveryPaths = await afterDiscoveryAuth.snapshot().0
        check(afterDiscoveryAttempt.outcome == .cancelled && afterDiscoveryPaths.isEmpty,
              "A's campus proof cannot authorize B's credential-bearing request")

        let duringDiscoveryBox = SelectionBox(.bound(expectedA))
        let duringDiscoveryAuth = FixturePhysicalSession(), duringDiscoveryProbe = FixturePhysicalSession()
        let duringDiscovery = client(duringDiscoveryBox, duringDiscoveryAuth, duringDiscoveryProbe)
        check(await duringDiscovery.setExpectedPhysicalRoute(expectedA), "A is admitted before an interrupted authentication page")
        await duringDiscoveryAuth.configure(afterRequest: { _ in duringDiscoveryBox.set(.bound(expectedB)) })
        let interrupted = await duringDiscovery.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .teaching)
        let interruptedPaths = await duringDiscoveryAuth.snapshot().0
        check(interrupted.outcome == .cancelled && interruptedPaths == ["/srun_portal_pc"],
              "A route change during the authentication homepage stops before challenge or credentials")

        // The caller can select a remembered secondary interface while the system primary is unknown.
        let secondary = CampusPhysicalRoute(interfaceName: "fixture-en12", interfaceIndex: 112,
            addresses: ["198.51.100.10"], dnsServers: ["198.51.100.53"], identity: "secondary-B")
        let primaryBox = SelectionBox(.unavailable), selectedBox = SelectionBox(.bound(secondary))
        let secondaryAuth = FixturePhysicalSession(), secondaryProbe = FixturePhysicalSession()
        let secondaryRoutes = RouteRecordBox()
        let secondaryClient = PortalClient(protocolClasses: [RefusingSystemProtocol.self], probeTimeout: 0.2,
            authenticationTimeout: 0.2, physicalSelectionProvider: { primaryBox.get() },
            targetedRouteProvider: { name in name == secondary.interfaceName ? selectedBox.get() : .unavailable },
            physicalTransportFactory: { route, cookies in
                secondaryRoutes.record(route.interfaceName)
                return cookies ? secondaryAuth : secondaryProbe
            })
        check(await secondaryClient.setExpectedPhysicalRoute(secondary), "A caller-admitted secondary interface need not be the system default")
        check(await secondaryClient.probe(mode: .complete).allPassed, "Fixed probes stay on the selected secondary interface")
        primaryBox.set(.bound(expectedA))
        check(await secondaryClient.discoverCampus(area: .automatic).teaching == .confirmed,
              "An unrelated primary-network change cannot replace the selected secondary lease")
        let secondaryAttempt = await secondaryClient.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        check(secondaryAttempt.outcome == .success && secondaryRoutes.snapshot() == [secondary.interfaceName, secondary.interfaceName],
              "Authentication and probes create only the exact admitted secondary transport")
        selectedBox.set(.unavailable)
        _ = await secondaryClient.probe(mode: .complete)
        check(await secondaryAuth.snapshot().0.filter { $0 == "/eportal/portal/login" }.count == 1,
              "A vanished selected interface never borrows the remaining system primary")

        // This uses the real physical-proof builder, not manually fabricated lease strings.
        // Refreshing a proxy's route table must not cancel an unchanged, source-bound login.
        let stableDefault = NetworkContinuitySnapshot.PhysicalDefault(interface: "fixture-en7", gateway: "192.0.2.1")
        let stableService = CampusPhysicalNetwork.Service(interface: "fixture-en7", router: "192.0.2.1",
            addresses: ["192.0.2.7"], masks: ["255.255.255.0"], linkActive: true,
            dhcpDNS: ["192.0.2.53"], serviceDNS: [], evidence: "fixture-selected-record",
            descriptor: .init(kind: "wifi", raw: ["gwip": "192.0.2.1", "net": "192.0.2.0/24"]))
        @Sendable func stableSelection(tunnel: String) -> CampusPhysicalSelection {
            CampusPhysicalNetwork.select(.init(primaryService: "fixture-service", primaryInterface: "fixture-en7",
                indices: ["fixture-en7": 7], liveAddresses: ["fixture-en7": ["192.0.2.7"]],
                services: ["fixture-service": stableService], routes: .init(identityComponents: [tunnel],
                    physicalDefaults: [stableDefault], boundPhysicalDefaults: [stableDefault]),
                globalEvidence: "fixture-global"), interfaceName: "fixture-en7")
        }
        guard case .bound(let stableRoute) = stableSelection(tunnel: "fixture-tun-before") else {
            preconditionFailure("fixture physical evidence must produce a bound route")
        }
        let stableBox = SelectionBox(.bound(stableRoute))
        let stableAuth = FixturePhysicalSession(), stableProbe = FixturePhysicalSession()
        let stableClient = PortalClient(protocolClasses: [RefusingSystemProtocol.self], probeTimeout: 0.2,
            authenticationTimeout: 0.2, physicalSelectionProvider: { stableBox.get() },
            targetedRouteProvider: { name in name == stableRoute.interfaceName ? stableBox.get() : .unavailable },
            physicalTransportFactory: { _, cookies in cookies ? stableAuth : stableProbe })
        check(await stableClient.setExpectedPhysicalRoute(stableRoute), "The selected physical lease is admitted before TUN churn")
        await stableProbe.configure(afterRequest: { path in
            if path == "/eportal" { stableBox.set(stableSelection(tunnel: "fixture-tun-after")) }
        })
        let stableDiscovery = await stableClient.discoverCampus(area: .automatic)
        check(await stableProbe.snapshot().0.contains("/eportal"), "The fixture changes the TUN route after a real read-only physical homepage response")
        check(stableDiscovery.teaching == .confirmed && stableDiscovery.dormitory == .confirmed,
              "TUN route churn preserves fresh campus proof on the unchanged physical lease")
        let stableAttempt = await stableClient.attemptLogin(credentials: Credentials(account: "fixture-account", password: "fixture-password"), area: .dormitory)
        check(stableAttempt.outcome == .success, "TUN-only changes during the credential-free homepage do not cancel an unchanged physical login")
        check(await stableAuth.snapshot().0.filter { $0 == "/eportal/portal/login" }.count == 1,
              "An unchanged physical lease completes one submission without another trust or login round")
        let stableAuthCancels = await stableAuth.snapshot().1, stableProbeCancels = await stableProbe.snapshot().1
        check(stableAuthCancels == 0 && stableProbeCancels == 0,
              "TUN-only changes retain the existing physical cookie scopes")
        primaryBox.set(.bound(expectedA))
        check(await secondaryClient.setExpectedPhysicalRoute(nil), "Clearing caller expectation explicitly restores legacy route selection")
        check(await secondaryClient.probe(mode: .complete).allPassed, "Legacy probes can recover only after expectation is explicitly cleared")

        let slowProbe = FixturePhysicalSession(), slowAuth = FixturePhysicalSession()
        await slowProbe.configure(delay: .milliseconds(60))
        let slow = client(SelectionBox(.bound(route("slow"))), slowAuth, slowProbe)
        let pending = Task { await slow.discoverCampus(area: .automatic) }
        for _ in 0..<100 {
            if await !slowProbe.snapshot().0.isEmpty { break }
            try await Task.sleep(for: .milliseconds(1))
        }
        await slow.resetConnections()
        let stale = await pending.value
        check(stale.teaching != .confirmed && stale.dormitory == .notChecked,
              "A late old-generation physical response cannot publish campus proof after reset")
        check(RefusingSystemProtocol.hits == 0, "All physical-routing fixtures remained isolated from the OS transport")
        print("CampusPhysicalRoutingTests: \(assertions) assertions passed; isolated transports only")
    }
}
