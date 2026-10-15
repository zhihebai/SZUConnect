import Foundation

/// Intercepts every scheme and host, including mistakes, so no test can reach a socket.
private final class OfflineProtocol: URLProtocol, @unchecked Sendable {
    enum Reply {
        case body(Data, status: Int = 200, delay: TimeInterval = 0, headers: [String: String] = [:])
        case failure(URLError.Code, delay: TimeInterval = 0)
        case redirect
        case hang
        case stalledBody
        /// The first request for this key since reset fails; later requests succeed.
        case failOnce(URLError.Code, then: Data)
    }
    private static let lock = NSLock()
    private static var replies: [String: Reply] = [:]
    private static var starts: [String] = []
    // Test-only virtual requests, retained in memory for parameter assertions and never printed.
    private static var requestURLs: [URL] = []
    private static var active: Set<UUID> = []
    private static var peak = 0
    private static var stopped = 0
    private let id = UUID()
    private let queue = DispatchQueue(label: "szu.offline.fixture")
    private var work: DispatchWorkItem?
    private var ended = false

    static func waitForIdle() async throws {
        for _ in 0..<100 {
            if isIdle { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        precondition(isIdle, "Transport cancellation did not release URLProtocol")
    }
    static func waitForStarts(_ keys: [String]) async throws -> Bool {
        for _ in 0..<200 {
            if keys.allSatisfy({ snapshot().starts.contains($0) }) { return true }
            try await Task.sleep(for: .milliseconds(2))
        }
        return false
    }
    private static var isIdle: Bool {
        lock.lock(); defer { lock.unlock() }
        return active.isEmpty
    }
    static func reset(_ fixtures: [String: Reply]) {
        lock.lock(); defer { lock.unlock() }
        precondition(active.isEmpty, "Previous offline request did not finish")
        replies = fixtures; starts = []; requestURLs = []; peak = 0; stopped = 0
    }
    static func snapshot() -> (starts: [String], peak: Int, stopped: Int, requestURLs: [URL]) {
        lock.lock(); defer { lock.unlock() }
        return (starts, peak, stopped, requestURLs)
    }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        let key = (request.url?.host ?? "") + (request.url?.path ?? "")
        Self.lock.lock()
        let stored = Self.replies[key] ?? .failure(.unsupportedURL)
        Self.starts.append(key)
        if let url = request.url { Self.requestURLs.append(url) }
        let attempt = Self.starts.filter { $0 == key }.count
        Self.active.insert(id); Self.peak = max(Self.peak, Self.active.count)
        Self.lock.unlock()
        let reply: Reply
        if case .failOnce(let code, let data) = stored {
            reply = attempt == 1 ? .failure(code) : .body(data)
        } else {
            reply = stored
        }
        let delay: TimeInterval
        switch reply {
        case .body(_, _, let seconds, _), .failure(_, let seconds): delay = seconds
        case .hang: return
        case .redirect, .stalledBody, .failOnce: delay = 0
        }
        let item = DispatchWorkItem { [weak self] in
            guard let self, !self.ended else { return }
            let url = self.request.url!
            switch reply {
            case .body(let data, let status, _, let headers):
                let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers)!
                self.client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
                self.client?.urlProtocol(self, didLoad: data)
                self.finish()
                self.client?.urlProtocolDidFinishLoading(self)
            case .failure(let code, _):
                self.finish()
                self.client?.urlProtocol(self, didFailWithError: URLError(code))
            case .redirect:
                let destination = URL(string: "https://unapproved.invalid/credential-trap")!
                let response = HTTPURLResponse(url: url, statusCode: 302, httpVersion: "HTTP/1.1", headerFields: ["Location": destination.absoluteString])!
                self.client?.urlProtocol(self, wasRedirectedTo: URLRequest(url: destination), redirectResponse: response)
                self.client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
                self.finish()
                self.client?.urlProtocolDidFinishLoading(self)
            case .stalledBody:
                let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: [:])!
                self.client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
                self.client?.urlProtocol(self, didLoad: Data("partial".utf8))
            case .failOnce(let code, _):
                self.finish()
                self.client?.urlProtocol(self, didFailWithError: URLError(code))
            case .hang: break
            }
        }
        work = item
        queue.asyncAfter(deadline: .now() + delay, execute: item)
    }
    private func finish() {
        guard !ended else { return }
        ended = true
        Self.lock.lock(); Self.active.remove(id); Self.lock.unlock()
    }
    override func stopLoading() {
        queue.sync {
            self.work?.cancel()
            if !self.ended {
                Self.lock.lock(); Self.stopped += 1; Self.lock.unlock()
                self.finish()
            }
        }
    }
}

/// Counts expensive physical snapshots, while all fixture requests remain in memory.
private final class LeaseCaptureMeter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    private var current = true
    let route = CampusPhysicalRoute(interfaceName: "fixture-en0", interfaceIndex: 99,
        addresses: ["192.0.2.10"], dnsServers: ["192.0.2.53"], identity: "fixture-lease")
    func capture() -> CampusPhysicalSelection { lock.withLock { count += 1; return current ? .bound(route) : .unavailable } }
    func reset() { lock.withLock { count = 0 } }
    func invalidate() { lock.withLock { current = false } }
    var captures: Int { lock.withLock { count } }
}

private actor CaptureFixtureSession: PortalPhysicalSession {
    var portalProbe = false
    var afterDormitory: (@Sendable () -> Void)?
    func usePortalProbe() { portalProbe = true }
    func invalidateOnDormitory(_ operation: @escaping @Sendable () -> Void) { afterDormitory = operation }
    func data(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
        let url = request.url!
        if portalProbe && url.host == "www.apple.com" { try await Task.sleep(for: .seconds(5)) }
        let body = url.path == "/srun_portal_pc" ? "<html>深圳大学 srun login</html>" : "<title>ePortal</title>"
        let redirect = portalProbe && url.host == "www.msftconnecttest.com"
        if url.path == "/eportal" { afterDormitory?() }
        return (Data(body.utf8), HTTPURLResponse(url: url, statusCode: redirect ? 302 : 200,
            httpVersion: "HTTP/1.1", headerFields: redirect ? ["Location": "http://172.30.255.42:801/eportal"] : nil)!)
    }
    func cancel() {}
}

@main
struct PortalTransportTests {
    static func main() async throws {
        setbuf(stdout, nil)
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1; print("PASS: \(name)")
        }
        let apple = "www.apple.com/library/test/success.html"
        let microsoft = "www.msftconnecttest.com/connecttest.txt"
        let page = "net.szu.edu.cn/srun_portal_pc"
        let challenge = "net.szu.edu.cn/cgi-bin/get_challenge"
        let submit = "net.szu.edu.cn/cgi-bin/srun_portal"
        let dormPage = "172.30.255.42/eportal"
        let dormSubmit = "172.30.255.42/eportal/portal/login"
        let appleOK = Data("<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>".utf8)
        let campusHTML = Data("深圳大学 srun login ac_id=1".utf8)
        let challengeOK = Data(#"{"error":"ok","challenge":"0123456789abcdef0123456789abcdef","client_ip":"10.2.3.4"}"#.utf8)
        let credentials = Credentials(account: "offline-student", password: "offline-test-password")
        let client = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 0.3, authenticationTimeout: 0.3)

        OfflineProtocol.reset([apple: .body(appleOK), microsoft: .body(Data("Microsoft Connect Test".utf8))])
        let systemResult = await PortalClient.probeSystemConnectivity(mode: .complete, protocolClasses: [OfflineProtocol.self])
        check(systemResult.allPassed, "System display probes validate both expected connectivity bodies")
        check(Set(OfflineProtocol.snapshot().starts) == Set([apple, microsoft]),
              "System display checks only public endpoints and never enter campus discovery or submission")
        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .body(campusHTML), microsoft: .body(campusHTML)])
        let captiveSystem = await PortalClient.probeSystemConnectivity(mode: .complete, protocolClasses: [OfflineProtocol.self])
        check(!captiveSystem.online, "HTTP 200 with portal content does not become system connectivity success")
        try await OfflineProtocol.waitForIdle()

        let leaseMeter = LeaseCaptureMeter(), captureFixture = CaptureFixtureSession()
        let measuredClient = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 0.8,
            authenticationTimeout: 0.8, physicalSelectionProvider: { .bound(leaseMeter.route) },
            targetedRouteProvider: { _ in leaseMeter.capture() }, physicalTransportFactory: { _, _ in captureFixture })
        let measuredAdmitted = await measuredClient.setExpectedPhysicalRoute(leaseMeter.route)
        check(measuredAdmitted, "Snapshot cost measurement uses a caller-admitted exact physical lease")
        leaseMeter.reset()
        let measuredDiscovery = await measuredClient.discoverCampus(area: .automatic)
        check(measuredDiscovery.teachingConfirmed && measuredDiscovery.dormitoryConfirmed,
              "Snapshot reduction preserves both fresh campus identity responses")
        check(leaseMeter.captures <= 4,
              "Two-request discovery performs at most one fresh caller lease capture per uninterrupted segment")
        await captureFixture.usePortalProbe()
        let physicalPortalStart = Date()
        let physicalPortal = await measuredClient.probe(mode: .quick)
        check(Date().timeIntervalSince(physicalPortalStart) < 0.3 && !physicalPortal.online && physicalPortal.microsoftResult.campusRedirect,
              "Physical-route probes also finish early for fixed campus redirects without claiming connectivity")
        check(physicalPortal.appleResult.state == .cancelled,
              "Physical-route portal recognition cancels the stalled peer honestly")

        let changedMeter = LeaseCaptureMeter(), changedFixture = CaptureFixtureSession()
        let changedClient = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 0.8,
            authenticationTimeout: 0.8, physicalSelectionProvider: { .bound(changedMeter.route) },
            targetedRouteProvider: { _ in changedMeter.capture() }, physicalTransportFactory: { _, _ in changedFixture })
        let changedAdmitted = await changedClient.setExpectedPhysicalRoute(changedMeter.route)
        check(changedAdmitted, "A secondary discovery race begins on an admitted lease")
        await changedFixture.invalidateOnDormitory { changedMeter.invalidate() }
        let changedDiscovery = await changedClient.discoverCampus(area: .automatic)
        check(!changedDiscovery.teachingConfirmed && !changedDiscovery.dormitoryConfirmed,
              "A lease change during the second discovery request invalidates all earlier campus proof")

        for reply in [#"{}"#, #"{"error":"ok","res":"failed"}"#] {
            try await OfflineProtocol.waitForIdle()
            OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK),
                                   submit: .body(Data(reply.utf8)), dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
            let attempt = await client.attemptLogin(credentials: credentials, area: .teaching)
            check(attempt.outcome == .uncertain, "Missing or contradictory teaching acknowledgement stays uncertain")
            check(attempt.diagnostic?.stage == .submission && attempt.diagnostic?.path == .teachingSubmit
                  && attempt.diagnostic?.httpStatus == 200 && attempt.diagnostic?.acID == "1",
                  "Teaching submission carries only bounded protocol evidence")
            check(OfflineProtocol.snapshot().starts.filter { $0 == submit }.count == 1
                  && !OfflineProtocol.snapshot().starts.contains(dormSubmit),
                  "Uncertain teaching reply neither retries nor changes area")
        }
        for body in [#"{"error":"ok","res":"ok","st":1,"suc_msg":"ip_already_online_error"}"#,
                     #"{"error":"login_error","error_msg":"ip_already_online_error"}"#] {
            for classified in [true, false] {
                try await OfflineProtocol.waitForIdle()
                OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK), submit: .body(Data(body.utf8))])
                if classified {
                    let result = await client.attemptLogin(credentials: credentials, area: .teaching)
                    check(result.outcome == .uncertain && result.diagnostic?.code == .existingSession
                          && result.diagnostic?.serverCode == .alreadyOnline,
                          "Existing IP session remains uncertain with bounded diagnostic evidence")
                } else {
                    do {
                        _ = try await client.login(credentials: credentials, area: .automatic)
                        fatalError("FAIL: already-online is not a credential success")
                    } catch PortalError.authenticationUncertain {
                        check(true, "Legacy login also classifies an existing IP session as uncertain")
                    }
                }
                check(OfflineProtocol.snapshot().starts.filter { $0 == submit }.count == 1
                      && !OfflineProtocol.snapshot().starts.contains(dormSubmit),
                      "Existing session never triggers another login or alternate-area submission")
            }
        }
        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK),
                               submit: .body(Data(#"{"error":"E2553","error_msg":"password is error"}"#.utf8))])
        let explicitRejection = await client.attemptLogin(credentials: credentials, area: .teaching)
        check(explicitRejection.outcome == .rejected && explicitRejection.rejection == .password
              && explicitRejection.diagnostic?.code == .explicitRejection
              && explicitRejection.diagnostic?.serverCode == .e2553,
              "Explicit password refusal retains credential classification")

        for reply in [#"{}"#, #"{"result":0,"msg":"认证失败"}"#] {
            try await OfflineProtocol.waitForIdle()
            OfflineProtocol.reset([dormSubmit: .body(Data(reply.utf8))])
            let attempt = await client.attemptLogin(credentials: credentials, area: .dormitory)
            check(attempt.outcome == .uncertain && attempt.diagnostic?.path == .dormitorySubmit,
                  "Missing or contradictory dormitory acknowledgement stays uncertain")
            check(OfflineProtocol.snapshot().starts.filter { $0 == dormSubmit }.count == 1,
                  "Uncertain dormitory reply makes one submission")
        }
        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([dormSubmit: .body(Data(#"{"result":1,"msg":"认证失败"}"#.utf8))])
        let dormRejection = await client.attemptLogin(credentials: credentials, area: .dormitory)
        check(dormRejection.outcome == .rejected && dormRejection.diagnostic?.code == .explicitRejection,
              "Explicit dormitory refusal remains rejected")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .body(appleOK, delay: 0.04), microsoft: .hang])
        let started = Date()
        let early = await client.probe()
        check(early.online && early.appleResult.state == .success, "One successful endpoint establishes connectivity")
        check(Date().timeIntervalSince(started) < 0.25, "Success returns without waiting for stalled peer")
        check(early.microsoftResult.state == .cancelled || early.microsoftResult.state == .notCompleted, "Cancelled peer is not reported as a failure")
        check(OfflineProtocol.snapshot().peak == 2, "Connectivity requests overlap")
        // URLSession can complete cancellation before URLProtocol's stopLoading callback is delivered.
        // Keep the latency assertion above; separately wait for bounded resource cleanup.
        try await OfflineProtocol.waitForIdle()
        check(OfflineProtocol.snapshot().stopped == 1, "First success cancels the stalled transport")
        check(early.appleResult.elapsed > 0, "Endpoint report records elapsed time")

        try await OfflineProtocol.waitForIdle()
        let portalQuickClient = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 0.8, authenticationTimeout: 0.8)
        OfflineProtocol.reset([apple: .hang, microsoft: .body(Data(), status: 302, delay: 0.02,
            headers: ["Location": "https://net.szu.edu.cn/srun_portal_pc?ac_id=1"])])
        let portalStarted = Date()
        let earlyPortal = await portalQuickClient.probe(mode: .quick)
        check(Date().timeIntervalSince(portalStarted) < 0.3,
              "A fixed campus portal redirect returns without waiting for the stalled quick-probe peer")
        check(!earlyPortal.online && earlyPortal.microsoftResult.state == .redirect,
              "A fixed campus portal redirect remains offline and requests fresh campus proof")
        check(earlyPortal.microsoftResult.campusRedirect,
              "Fixed portal recognition exposes only a boolean recovery hint")
        check(earlyPortal.appleResult.state == .cancelled,
              "A peer cancelled by campus redirection stays honestly cancelled")
        try await OfflineProtocol.waitForIdle()
        check(OfflineProtocol.snapshot().starts.allSatisfy { $0 == apple || $0 == microsoft },
              "Quick portal recognition never follows or requests the Location destination")

        let fixedPortalLocations = ["https://net.szu.edu.cn", "https://net.szu.edu.cn/",
            "https://net.szu.edu.cn:443/srun_portal_pc?ac_id=1", "http://172.30.255.42:801/",
            "http://172.30.255.42:801/eportal?offline=1"]
        let probeOrigin = URL(string: "http://www.msftconnecttest.com/connecttest.txt")!
        for location in fixedPortalLocations {
            check(CampusRedirectTarget.matches(location, relativeTo: probeOrigin),
                  "Existing fixed portal redirect whitelist is preserved")
        }
        var fakeUserInfoPortal = URLComponents(string: "https://net.szu.edu.cn/srun_portal_pc")!
        fakeUserInfoPortal.user = "fixture-user"
        fakeUserInfoPortal.password = "fixture-password"
        let rejectedPortalLocations = ["", "https://unapproved.invalid/srun_portal_pc",
            "https://net.szu.edu.cn.evil.invalid/srun_portal_pc", "http://net.szu.edu.cn/srun_portal_pc",
            "https://net.szu.edu.cn:444/srun_portal_pc", "http://172.30.255.42/eportal",
            fakeUserInfoPortal.string!, "https://net.szu.edu.cn/unknown",
            "https://net.szu.edu.cn/srun%5fportal_pc", "https://net.szu.edu.cn/%73run_portal_pc",
            "https://net.szu.edu.cn/a/../srun_portal_pc", "https://net.szu.edu.cn/%2e%2e/srun_portal_pc",
            "https://net.szu.edu.cn/\\srun_portal_pc", "https://net.szu.edu.cn/srun_portal_pc\n",
            "https://net.szu.edu.cn/srun_portal_pc?" + String(repeating: "x", count: 2048)]
        check(!CampusRedirectTarget.matches(nil, relativeTo: probeOrigin), "Missing Location cannot shorten a quick probe")
        for location in rejectedPortalLocations {
            check(!CampusRedirectTarget.matches(location, relativeTo: probeOrigin),
                  "Unknown, malformed, credential-bearing or encoded portal target cannot shorten a probe")
        }

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .hang, microsoft: .body(Data(), status: 302,
            headers: ["Location": "https://unapproved.invalid/srun_portal_pc"])])
        let unknownPortalStarted = Date()
        let unknownPortal = await client.probe(mode: .quick)
        check(Date().timeIntervalSince(unknownPortalStarted) >= 0.24,
              "An unknown redirect still waits for the other quick-probe endpoint")
        check(unknownPortal.appleResult.state == .timeout && !unknownPortal.microsoftResult.campusRedirect,
              "An unknown redirect retains the real peer timeout and no fixed-campus hint")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .hang, microsoft: .body(Data(), status: 302,
            headers: ["Location": "http://172.30.255.42:801/eportal"])])
        let fullPortalStarted = Date()
        let fullPortal = await client.probe(mode: .complete)
        check(Date().timeIntervalSince(fullPortalStarted) >= 0.24,
              "Complete probes still wait for both endpoints even after a fixed portal redirect")
        check(fullPortal.appleResult.state == .timeout && fullPortal.microsoftResult.campusRedirect && !fullPortal.online,
              "Complete portal probes preserve actual timeout evidence for uncertain-login checks")
        check(OfflineProtocol.snapshot().starts.allSatisfy { $0 == apple || $0 == microsoft },
              "Neither unknown nor known redirect causes a destination request")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .body(Data(), status: 302,
            headers: ["Location": "https://net.szu.edu.cn/srun_portal_pc"]), microsoft: .body(Data(), status: 503)])
        let reversePortal = await client.probe(mode: .quick)
        check(reversePortal.appleResult.campusRedirect && !reversePortal.online,
              "A fixed portal redirect from either base endpoint can finish a quick probe")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(Data(), status: 302,
            headers: ["Location": "https://net.szu.edu.cn/srun_portal_pc"])])
        let redirectedDiscovery = await client.discoverCampus(area: .teaching)
        check(!redirectedDiscovery.teachingConfirmed && OfflineProtocol.snapshot().starts == [page],
              "Recognized portal redirects do not become campus discovery proof")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .body(appleOK, delay: 0.02),
                               microsoft: .body(Data("Microsoft Connect Test".utf8), delay: 0.14)])
        let complete = await client.probe(mode: .complete)
        check(complete.mode == .complete && complete.allPassed && complete.passedCount == 2,
              "Complete mode waits for both independently valid probe responses")
        check(complete.microsoftResult.elapsed >= 0.10, "Complete mode does not cancel its slower valid peer")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .body(appleOK), microsoft: .hang])
        let partial = await client.probe(mode: .complete)
        check(partial.online && !partial.allPassed && partial.passedCount == 1,
              "A partial complete check keeps basic connectivity evidence without claiming all passed")
        check(partial.microsoftResult.state == .timeout,
              "Complete mode reports a real timeout rather than a peer being skipped")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .hang, microsoft: .hang])
        let fullTask = Task { await client.probe(mode: .complete) }
        try await Task.sleep(for: .milliseconds(25))
        fullTask.cancel()
        let fullCancelled = await fullTask.value
        check(fullCancelled.appleResult.state == .cancelled && fullCancelled.microsoftResult.state == .cancelled,
              "Cancelling a complete check cancels both transports without a false skipped status")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .failure(.timedOut), microsoft: .failure(.cannotFindHost)])
        let failures = await client.probe()
        check(failures.appleResult.state == .timeout && failures.microsoftResult.state == .dns, "Timeout and DNS failures remain distinct")
        check(!failures.online, "Two transport failures do not report online")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .failure(.serverCertificateUntrusted), microsoft: .body(Data("portal login".utf8))])
        let mismatch = await client.probe()
        check(mismatch.appleResult.state == .tls && mismatch.microsoftResult.state == .unexpectedResponse, "TLS and captive page diagnoses remain distinct")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .redirect, microsoft: .body(Data(), status: 503)])
        let redirected = await client.probe()
        check(redirected.appleResult.state == .redirect, "URLSession redirect delegate refuses an unapproved destination")
        check(!OfflineProtocol.snapshot().starts.contains(where: { $0.contains("unapproved.invalid") }), "Redirected destination receives no request")
        check(redirected.microsoftResult.state == .unexpectedResponse, "Non-200 status is rejected")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .body(Data(repeating: 65, count: 65_537)), microsoft: .body(Data(), status: 204)])
        let oversized = await client.probe()
        check(!oversized.online && oversized.appleResult.state == .unexpectedResponse, "Oversized probe content is bounded and rejected")

        // Cross both the 4 KiB aggregation boundary and the 1 MiB discovery budget.
        // An absent Content-Length must still be bounded by the actual received bytes.
        let bodyLimitClient = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 3, authenticationTimeout: 3)
        for size in [4095, 4096, 4097, 1_048_576, 1_048_577] {
            try await OfflineProtocol.waitForIdle()
            var body = campusHTML
            body.append(Data(repeating: 32, count: size - body.count))
            OfflineProtocol.reset([page: .body(body)])
            let result = await bodyLimitClient.discoverCampus(area: .teaching)
            check(result.teachingConfirmed == (size <= 1_048_576),
                  "Discovery body boundary \(size) preserves exact bytes and the receive limit without Content-Length")
            check(OfflineProtocol.snapshot().starts == [page],
                  "Discovery body boundary \(size) stays credential-free and never retries oversized input")
        }

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .hang, microsoft: .hang])
        let deadlineStart = Date()
        let timedOut = await client.probe()
        check(timedOut.appleResult.state == .timeout && timedOut.microsoftResult.state == .timeout, "Hard deadline also stops a transport that never calls back")
        check(Date().timeIntervalSince(deadlineStart) < 1, "Probe deadline bounds stalled requests")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .stalledBody, microsoft: .hang])
        let stalledBody = await client.probe()
        check(stalledBody.appleResult.state == .timeout, "Hard deadline bounds a body stalled after response headers")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .hang, microsoft: .hang])
        let cancelledTask = Task { await client.probe() }
        try await Task.sleep(for: .milliseconds(25))
        cancelledTask.cancel()
        let cancelled = await cancelledTask.value
        check(!cancelled.online && cancelled.appleResult.state == .cancelled && cancelled.microsoftResult.state == .cancelled, "Caller cancellation stops every probe")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK), submit: .failure(.timedOut), dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        do {
            _ = try await client.login(credentials: credentials, area: .automatic)
            fatalError("FAIL: uncertain authentication must not fall through")
        } catch PortalError.authenticationUncertain {
            check(!OfflineProtocol.snapshot().starts.contains(dormSubmit), "Submitted teaching authentication cannot leak into dormitory fallback")
            check(OfflineProtocol.snapshot().starts.filter { $0 == submit }.count == 1,
                  "A timed-out credential submission is never retried")
        }

        // A failed or missing controller must never become a password submission to controller 1.
        for failedHome in [OfflineProtocol.Reply.failure(.timedOut), .failure(.cannotFindHost),
                           .body(Data("temporary failure".utf8), status: 503),
                           .body(Data("深圳大学 srun login without controller".utf8))] {
            for classified in [true, false] {
                try await OfflineProtocol.waitForIdle()
                OfflineProtocol.reset([page: failedHome, challenge: .body(challengeOK),
                                       submit: .body(Data(#"{"error":"ok"}"#.utf8))])
                if classified {
                    let result = await client.attemptLogin(credentials: credentials, area: .teaching)
                    check(result.outcome == .unavailable || result.outcome == .security,
                          "Missing controller returns a pre-submission failure")
                } else {
                    do {
                        _ = try await client.login(credentials: credentials, area: .teaching)
                        fatalError("FAIL: legacy teaching must require a controller")
                    } catch { check(true, "Legacy teaching rejects missing controller before password material") }
                }
                check(!OfflineProtocol.snapshot().starts.contains(challenge) && !OfflineProtocol.snapshot().starts.contains(submit),
                      "Homepage failure or absent controller sends neither challenge nor password")
            }
        }

        // Credential-free reads allow one pause and retry before any submission.
        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .failOnce(.timedOut, then: challengeOK),
                               submit: .body(Data(#"{"error":"ok"}"#.utf8))])
        let retried = try await client.login(credentials: credentials, area: .teaching)
        check(retried.contains("教学"), "A transient challenge failure is retried once before submission")
        check(OfflineProtocol.snapshot().starts.filter { $0 == challenge }.count == 2
              && OfflineProtocol.snapshot().starts.filter { $0 == submit }.count == 1,
              "Retry repeats only the credential-free challenge; the submission is sent exactly once")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .failOnce(.networkConnectionLost, then: campusHTML)])
        let rediscovered = await client.confirmedCampusArea(area: .teaching)
        check(rediscovered == .teaching && OfflineProtocol.snapshot().starts.filter { $0 == page }.count == 2,
              "Read-only campus discovery survives one transient failure")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .failure(.serverCertificateUntrusted)])
        let untrusted = await client.confirmedCampusArea(area: .teaching)
        check(untrusted == nil && OfflineProtocol.snapshot().starts.filter { $0 == page }.count == 1,
              "Certificate failures are never retried")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([apple: .failOnce(.timedOut, then: appleOK), microsoft: .failure(.cannotFindHost)])
        let unretriedProbe = await client.probe()
        check(!unretriedProbe.online && OfflineProtocol.snapshot().starts.filter { $0 == apple }.count == 1,
              "Connectivity probes report what they saw and are never retried")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .failure(.cannotFindHost), dormPage: .body(Data("<title>EPortal</title>".utf8)), dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        let fallback = try await client.login(credentials: credentials, area: .automatic)
        check(fallback.contains("宿舍"), "Pre-submit availability failure can use a newly verified dormitory portal")
        check(OfflineProtocol.snapshot().starts.filter { $0 == dormSubmit }.count == 1, "Verified alternate area receives one authentication only")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(Data(#"{"error":"password_error"}"#.utf8)), dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        do {
            _ = try await client.login(credentials: credentials, area: .automatic)
            fatalError("FAIL: rejection must stop")
        } catch PortalError.rejected {
            check(!OfflineProtocol.snapshot().starts.contains(dormSubmit), "Explicit rejection never downgrades the area")
        }

        // A syntactically valid token is insufficient when the status fields disagree.
        let contradictoryChallenges: [[String: Any]] = [
            ["error": "ok", "res": "failed"],
            ["error": "denied", "res": "ok"],
            ["error": 0, "res": "ok"],
            ["error": "ok", "res": true],
            ["error": "", "res": "ok"],
            [:]
        ]
        for status in contradictoryChallenges {
            try await OfflineProtocol.waitForIdle()
            var object = status
            object["challenge"] = "0123456789abcdef0123456789abcdef"
            object["client_ip"] = "10.2.3.4"
            let body = try JSONSerialization.data(withJSONObject: object)
            OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(body),
                                   submit: .body(Data(#"{"error":"ok"}"#.utf8)),
                                   dormPage: .body(Data("<title>EPortal</title>".utf8)),
                                   dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
            do {
                _ = try await client.login(credentials: credentials, area: .automatic)
                fatalError("FAIL: invalid challenge status must block authentication")
            } catch PortalError.protocolConflict {
                let requests = OfflineProtocol.snapshot().starts
                check(!requests.contains(submit) && !requests.contains(dormSubmit),
                      "Conflicting or malformed challenge status sends credentials to neither area")
                check(requests.filter { $0 == challenge }.count == 1 && !requests.contains(dormPage),
                      "Challenge protocol conflict triggers neither retry nor alternate-area discovery")
            }
        }

        for invalidBody in [
            "<html>temporary portal error</html>", "[]",
            #"{"error":"ok","client_ip":"10.2.3.4"}"#,
            #"{"res":"ok","challenge":"token","client_ip":""}"#,
            #"{"error":"ok","challenge":42,"client_ip":"10.2.3.4"}"#
        ] {
            try await OfflineProtocol.waitForIdle()
            OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(Data(invalidBody.utf8)),
                                   dormPage: .body(Data("<title>EPortal</title>".utf8)),
                                   dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
            do {
                _ = try await client.login(credentials: credentials, area: .automatic)
                fatalError("FAIL: malformed challenge must not cause alternate credential submission")
            } catch PortalError.protocolConflict {
                check(OfflineProtocol.snapshot().starts == [page, challenge],
                      "Malformed or incomplete challenge stops before either area's credential submission")
            }
        }

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(Data("深圳大学 srun login ac_id=18&ac_id=27".utf8)),
                               challenge: .body(challengeOK), submit: .body(Data(#"{"error":"ok"}"#.utf8)),
                               dormPage: .body(Data("<title>EPortal</title>".utf8)),
                               dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        do {
            _ = try await client.login(credentials: credentials, area: .automatic)
            fatalError("FAIL: conflicting controller discovery must block authentication")
        } catch PortalError.protocolConflict {
            let requests = OfflineProtocol.snapshot().starts
            check(requests == [page], "Conflicting controllers stop before challenge, submission or alternate-area discovery")
        }

        try await OfflineProtocol.waitForIdle()
        let controllerPage = Data(#"深圳大学 srun login <input value="18" name="ac_id">"#.utf8)
        let resChallenge = Data(#"{"res":"ok","challenge":"0123456789abcdef0123456789abcdef","client_ip":"10.2.3.4"}"#.utf8)
        OfflineProtocol.reset([page: .body(controllerPage), challenge: .body(resChallenge),
                               submit: .body(Data(#"{"res":"ok"}"#.utf8))])
        let compatible = try await client.login(credentials: credentials, area: .teaching)
        let teachingRequests = OfflineProtocol.snapshot().requestURLs.filter { $0.path == "/cgi-bin/srun_portal" }
        check(compatible.contains("教学") && teachingRequests.count == 1,
              "SRun res-only success and an input controller reach exactly one submission")
        let teachingQuery = URLComponents(url: teachingRequests[0], resolvingAgainstBaseURL: false)?.queryItems ?? []
        check(teachingQuery.filter { $0.name == "ac_id" }.map(\.value) == ["18"],
              "The discovered controller is used in the actual SRun request")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(Data("深圳大学 srun login acid   : \"6\"".utf8)),
                               challenge: .body(challengeOK), submit: .body(Data(#"{"error":"ok"}"#.utf8))])
        let liveAlias = await client.attemptLogin(credentials: credentials, area: .teaching)
        let aliasRequests = OfflineProtocol.snapshot().requestURLs.filter { $0.path == "/cgi-bin/srun_portal" }
        let aliasQuery = aliasRequests.first.flatMap { URLComponents(url: $0, resolvingAgainstBaseURL: false)?.queryItems } ?? []
        check(liveAlias.outcome == .success && liveAlias.diagnostic?.acID == "6" && aliasRequests.count == 1,
              "School-page acid alias is captured before one submission")
        check(aliasQuery.filter { $0.name == "ac_id" }.map(\.value) == ["6"],
              "The live acid alias controls the submitted ac_id instead of the default")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(Data("深圳大学 srun login acid:6; ac_id=1".utf8)),
                               challenge: .body(challengeOK), submit: .body(Data(#"{"error":"ok"}"#.utf8))])
        let aliasConflict = await client.attemptLogin(credentials: credentials, area: .teaching)
        check(aliasConflict.outcome == .security && OfflineProtocol.snapshot().starts == [page],
              "Conflicting acid aliases stop before challenge or credential submission")
        for invalidAlias in ["acid:0", "acid:6evil"] {
            try await OfflineProtocol.waitForIdle()
            OfflineProtocol.reset([page: .body(Data("深圳大学 srun login \(invalidAlias)".utf8)),
                                   challenge: .body(challengeOK), submit: .body(Data(#"{"error":"ok"}"#.utf8))])
            let invalid = await client.attemptLogin(credentials: credentials, area: .teaching)
            check(invalid.outcome == .security && OfflineProtocol.snapshot().starts == [page],
                  "Invalid acid alias blocks challenge and credential submission")
        }

        try await OfflineProtocol.waitForIdle()
        let specialCredentials = Credentials(account: "offline+&= ?#%中文", password: "test+&= ?#%密码")
        OfflineProtocol.reset([dormSubmit: .body(Data(#"jsonpReturn({"result":1,"msg":"认证成功"})"#.utf8))])
        let dormResult = try await client.login(credentials: specialCredentials, area: .dormitory)
        let dormRequests = OfflineProtocol.snapshot().requestURLs
        check(dormResult.contains("宿舍") && dormRequests.count == 1,
              "Dormitory authentication sends one request without discovery or retries")
        let dormURL = dormRequests[0]
        let dormQuery = URLComponents(url: dormURL, resolvingAgainstBaseURL: false)?.queryItems ?? []
        check(dormURL.scheme == "http" && dormURL.host == "172.30.255.42" && dormURL.port == 801
              && dormURL.path == "/eportal/portal/login", "Explicit login method preserves the fixed dormitory destination")
        check(dormQuery.filter { $0.name == "login_method" }.map(\.value) == ["1"],
              "The dormitory request contains login_method equal to one exactly once")
        check(dormQuery.filter { $0.name == "user_account" }.map(\.value) == [",0," + specialCredentials.account]
              && dormQuery.filter { $0.name == "user_password" }.map(\.value) == [specialCredentials.password],
              "Actual dormitory credential parameters preserve special characters without duplicate fields")
        check(dormURL.absoluteString.contains("%2B") && dormURL.absoluteString.contains("%26")
              && dormURL.absoluteString.contains("%23"), "Actual dormitory form encoding protects plus, ampersand and fragment characters")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .failure(.serverCertificateUntrusted), dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        do {
            _ = try await client.login(credentials: credentials, area: .automatic)
            fatalError("FAIL: TLS failure must stop")
        } catch PortalError.tls {
            check(!OfflineProtocol.snapshot().starts.contains(dormSubmit), "TLS failure never sends dormitory credentials")
        }
        // Foundation's custom URLProtocol bypasses the native HTTP cookie layer.
        // Cookie storage is therefore reviewed through the isolated session setup;
        // the fixture does not claim to validate native HTTP cookie persistence.
        for submissionReply in [OfflineProtocol.Reply.redirect,
                                .body(Data("unrecognized response".utf8)),
                                .body(Data(), headers: ["Content-Length": "1048577"])] {
            try await OfflineProtocol.waitForIdle()
            OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK), submit: submissionReply,
                                   dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
            do {
                _ = try await client.login(credentials: credentials, area: .automatic)
                fatalError("FAIL: uncertain submission must stop")
            } catch PortalError.authenticationUncertain {
                check(!OfflineProtocol.snapshot().starts.contains(dormSubmit), "Redirected, malformed or oversized authentication response never resubmits")
            }
        }

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .failure(.cannotFindHost),
                               dormPage: .body(Data("hotel login".utf8)), dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        do {
            _ = try await client.login(credentials: credentials, area: .automatic)
            fatalError("FAIL: unverified fallback must stop")
        } catch PortalError.dns {
            check(!OfflineProtocol.snapshot().starts.contains(dormSubmit), "Pre-submit failure cannot bypass alternate-area identity verification")
        }

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK), submit: .hang])
        let authenticationTask = Task { try await client.login(credentials: credentials, area: .automatic) }
        for _ in 0..<100 {
            if OfflineProtocol.snapshot().starts.contains(submit) { break }
            try await Task.sleep(for: .milliseconds(1))
        }
        check(OfflineProtocol.snapshot().starts.contains(submit), "Cancellation fixture reaches the authentication submission")
        authenticationTask.cancel()
        do {
            _ = try await authenticationTask.value
            fatalError("FAIL: cancelled authentication must stop")
        } catch is CancellationError {
            check(!OfflineProtocol.snapshot().starts.contains(dormSubmit), "Cancelling an active submission never authenticates another area")
        }
        try await OfflineProtocol.waitForIdle()

        OfflineProtocol.reset([page: .failure(.timedOut)])
        let retryClient = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 4, authenticationTimeout: 0.3)
        let discoveryTask = Task { await retryClient.confirmedCampusArea(area: .teaching) }
        for _ in 0..<100 {
            if OfflineProtocol.snapshot().starts.contains(page) { break }
            try await Task.sleep(for: .milliseconds(1))
        }
        try await OfflineProtocol.waitForIdle()
        discoveryTask.cancel()
        _ = await discoveryTask.value
        check(OfflineProtocol.snapshot().starts.filter { $0 == page }.count == 1,
              "Cancelling during the retry pause suppresses the second read")
        check(!OfflineProtocol.snapshot().starts.contains(submit) && !OfflineProtocol.snapshot().starts.contains(dormSubmit),
              "A cancelled retry cannot trigger a credential submission")

        // Replacing the transport must work without reconstructing the PortalClient or sending
        // a request itself. All fixtures still intercept every host after each replacement.
        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([:])
        let resetClient = PortalClient(protocolClasses: [OfflineProtocol.self], probeTimeout: 2, authenticationTimeout: 2)
        await resetClient.resetConnections()
        check(OfflineProtocol.snapshot().starts.isEmpty, "Resetting connections performs no read or credential submission")

        OfflineProtocol.reset([apple: .hang, microsoft: .hang])
        let oldProbeTask = Task { await resetClient.probe(mode: .complete) }
        let oldProbesStarted = try await OfflineProtocol.waitForStarts([apple, microsoft])
        check(oldProbesStarted, "Reset fixture reaches both stalled connectivity transports")
        let resetStarted = Date()
        await resetClient.resetConnections()
        let resetProbe = await oldProbeTask.value
        check(resetProbe.appleResult.state == .cancelled && resetProbe.microsoftResult.state == .cancelled,
              "Resetting sessions cancels both old probes without a false network failure")
        check(Date().timeIntervalSince(resetStarted) < 1,
              "Reset cancels a stalled probe before its two-second deadline")
        try await OfflineProtocol.waitForIdle()
        check(OfflineProtocol.snapshot().stopped == 2, "Reset releases both stalled URLProtocol transports")

        OfflineProtocol.reset([apple: .body(appleOK), microsoft: .body(Data("Microsoft Connect Test".utf8))])
        let freshProbe = await resetClient.probe(mode: .complete)
        check(freshProbe.allPassed, "The same client completes a fresh probe after resetting stalled sessions")
        check(OfflineProtocol.snapshot().starts.count == 2,
              "The replacement probe session retains protocol injection and sends each probe once")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK),
                               submit: .body(Data(#"{"error":"ok"}"#.utf8))])
        let freshLogin = try await resetClient.login(credentials: credentials, area: .teaching)
        check(freshLogin.contains("教学"), "The same client logs in after resetting stalled probes")
        check(OfflineProtocol.snapshot().starts == [page, challenge, submit],
              "Fresh authentication retains the page, challenge and one-submission sequence")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([:])
        for _ in 0..<5 { await resetClient.resetConnections() }
        check(OfflineProtocol.snapshot().starts.isEmpty, "Repeated resets remain request-free")
        OfflineProtocol.reset([apple: .body(appleOK), microsoft: .body(Data("Microsoft Connect Test".utf8)),
                               dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        let repeatedProbe = await resetClient.probe(mode: .complete)
        let repeatedLogin = try await resetClient.login(credentials: credentials, area: .dormitory)
        check(repeatedProbe.allPassed && repeatedLogin.contains("宿舍"),
              "Repeated resets do not poison subsequent probes or authentication")
        check(OfflineProtocol.snapshot().starts.filter { $0 == dormSubmit }.count == 1,
              "Authentication after repeated resets still submits exactly once")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .hang,
                               submit: .body(Data(#"{"error":"ok"}"#.utf8)),
                               dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        let oldLoginTask = Task { try await resetClient.login(credentials: credentials, area: .automatic) }
        let oldChallengeStarted = try await OfflineProtocol.waitForStarts([challenge])
        check(oldChallengeStarted, "Reset fixture pauses an existing login before credential submission")
        await resetClient.resetConnections()
        do {
            _ = try await oldLoginTask.value
            fatalError("FAIL: reset login must be cancelled")
        } catch is CancellationError {
            check(OfflineProtocol.snapshot().starts == [page, challenge],
                  "An old login cannot submit credentials or switch areas through the replacement sessions")
        }
        try await OfflineProtocol.waitForIdle()

        // Reset during the retry pause is the re-entrancy case: no URLSession task remains to
        // cancel, so the operation generation must reject its next read on the new sessions.
        OfflineProtocol.reset([page: .failOnce(.timedOut, then: campusHTML), challenge: .body(challengeOK),
                               submit: .body(Data(#"{"error":"ok"}"#.utf8))])
        let oldRetryTask = Task { await resetClient.attemptLogin(credentials: credentials, area: .teaching) }
        let retryReadStarted = try await OfflineProtocol.waitForStarts([page])
        check(retryReadStarted, "Reset fixture enters a credential-free retry pause")
        try await OfflineProtocol.waitForIdle()
        await resetClient.resetConnections()
        let resetRetry = await oldRetryTask.value
        check(resetRetry.outcome == .cancelled, "Reset invalidates a suspended login retry even without caller cancellation")
        check(OfflineProtocol.snapshot().starts == [page],
              "A retry from the old generation cannot read, challenge or submit through new sessions")

        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), dormPage: .hang])
        let oldDiscoveryTask = Task { await resetClient.discoverCampus(area: .automatic) }
        let dormReadStarted = try await OfflineProtocol.waitForStarts([dormPage])
        check(dormReadStarted, "Reset fixture obtains teaching proof before a stalled dormitory check")
        await resetClient.resetConnections()
        let resetDiscovery = await oldDiscoveryTask.value
        check(resetDiscovery.teaching == .unconfirmed && resetDiscovery.dormitory == .notChecked,
              "Reset discards all partial campus proof from the old network")
        try await OfflineProtocol.waitForIdle()

        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK), submit: .hang,
                               dormSubmit: .body(Data(#"{"result":0}"#.utf8))])
        let oldSubmissionTask = Task { await resetClient.attemptLogin(credentials: credentials, area: .teaching) }
        let oldSubmissionStarted = try await OfflineProtocol.waitForStarts([submit])
        check(oldSubmissionStarted, "Reset fixture reaches a classified authentication submission")
        await resetClient.resetConnections()
        let resetSubmission = await oldSubmissionTask.value
        check(resetSubmission.outcome == .cancelled, "Reset cancels the old classified authentication result")
        check(OfflineProtocol.snapshot().starts == [page, challenge, submit],
              "Resetting an active submission never resubmits or falls back")
        try await OfflineProtocol.waitForIdle()
        OfflineProtocol.reset([page: .body(campusHTML), challenge: .body(challengeOK),
                               submit: .body(Data(#"{"error":"ok"}"#.utf8))])
        let nextAttempt = await resetClient.attemptLogin(credentials: credentials, area: .teaching)
        check(nextAttempt.outcome == .success, "A new classified attempt succeeds without restarting the client")
        check(OfflineProtocol.snapshot().starts == [page, challenge, submit],
              "New classified authentication uses one full handshake and one submission")
        try await OfflineProtocol.waitForIdle()

        print("\(passed) offline URLProtocol transport assertions passed. All requests were intercepted in memory.")
    }
}
