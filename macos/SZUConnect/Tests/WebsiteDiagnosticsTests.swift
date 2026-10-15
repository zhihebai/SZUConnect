import Foundation

/// Intercept every request, including unexpected schemes/hosts, to prohibit sockets.
private final class WebsiteOfflineProtocol: URLProtocol, @unchecked Sendable {
    enum Reply {
        case headers(Int), redirect, hang, error(URLError.Code), cookie
    }
    private static let lock = NSLock()
    private static var reply: Reply = .hang
    private static var captured: [URLRequest] = []
    private static var active = 0
    private static var stopped = 0
    private let queue = DispatchQueue(label: "szu.website.fixture")
    private var ended = false

    static func reset(_ reply: Reply) {
        lock.lock(); defer { lock.unlock() }
        precondition(active == 0, "Previous request still active")
        self.reply = reply; captured = []; stopped = 0
    }
    static func snapshot() -> (requests: [URLRequest], active: Int, stopped: Int) {
        lock.lock(); defer { lock.unlock() }; return (captured, active, stopped)
    }
    static func idle() async throws {
        for _ in 0..<200 {
            if snapshot().active == 0 { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        precondition(snapshot().active == 0, "URLSession failed to cancel a header-only task")
    }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.lock.lock()
        let reply = Self.reply
        Self.captured.append(request); Self.active += 1
        Self.lock.unlock()
        queue.async { [self] in
            guard !ended else { return }
            let url = request.url!
            switch reply {
            case .headers(let status):
                let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1", headerFields: ["Content-Length": "9999999999"])!
                client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
                // Foundation buffers this custom protocol until finish. No body is provided.
                client?.urlProtocolDidFinishLoading(self)
            case .redirect:
                let target = URL(string: "https://redirect-target.invalid/private?secret=must-not-appear")!
                let response = HTTPURLResponse(url: url, statusCode: 302, httpVersion: "HTTP/1.1", headerFields: ["Location": target.absoluteString])!
                client?.urlProtocol(self, wasRedirectedTo: URLRequest(url: target), redirectResponse: response)
            case .error(let code):
                finish()
                client?.urlProtocol(self, didFailWithError: URLError(code, userInfo: [NSLocalizedDescriptionKey: "SECRET-TOKEN-DO-NOT-EXPOSE"]))
            case .cookie:
                let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: ["Set-Cookie": "diagnostic_cookie=should-not-persist; Path=/"])!
                client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .allowed)
                client?.urlProtocolDidFinishLoading(self)
            case .hang: break
            }
        }
    }
    private func finish() {
        guard !ended else { return }
        ended = true
        Self.lock.lock(); Self.active -= 1; Self.lock.unlock()
    }
    override func stopLoading() {
        queue.sync {
            if !ended {
                Self.lock.lock(); Self.stopped += 1; Self.lock.unlock()
                finish()
            }
        }
    }
}

private final class UnusedChallengeSender: NSObject, URLAuthenticationChallengeSender {
    func use(_ credential: URLCredential, for challenge: URLAuthenticationChallenge) {}
    func continueWithoutCredential(for challenge: URLAuthenticationChallenge) {}
    func cancel(_ challenge: URLAuthenticationChallenge) {}
    func performDefaultHandling(for challenge: URLAuthenticationChallenge) {}
    func rejectProtectionSpaceAndContinue(with challenge: URLAuthenticationChallenge) {}
}

@main
struct WebsiteDiagnosticsTests {
    static func main() async throws {
        setbuf(stdout, nil)
        var passed = 0
        func check(_ value: @autoclosure () -> Bool, _ label: String) {
            guard value() else { fatalError("FAIL: \(label)") }
            passed += 1; print("PASS: \(label)")
        }
        func normalized(_ input: String) -> String? { try? WebsiteAddress.normalize(input).absoluteString }
        check(normalized(" example.invalid ") == "https://example.invalid", "Missing scheme defaults to HTTPS")
        check(normalized("HTTP://EXAMPLE.INVALID:8080/a") == "http://example.invalid:8080/a", "Scheme and domain normalize without losing port")
        check(normalized("example.invalid:8443/测试 路径") == "https://example.invalid:8443/%E6%B5%8B%E8%AF%95%20%E8%B7%AF%E5%BE%84", "Unicode and spaces in path encode correctly")
        check(normalized("https://example.invalid/a%2Fb") == "https://example.invalid/a%2Fb", "Existing percent escapes are not double encoded")
        check(normalized("https://[2001:db8::1]/") == "https://[2001:db8::1]/", "IPv6 literal remains valid")
        for input in ["", "file:///tmp/private", "data:text/plain,secret", "ftp://example.invalid", "javascript:alert(1)", "https://user:pass@example.invalid", "https://example.invalid?token=secret", "https://example.invalid?", "https://example.invalid/#token", "https://example.invalid/#", "https://", "https://example.invalid:0", "https://example.invalid:65536", "https://evil.invalid\\@example.invalid", "https://example.invalid/\nsecret"] {
            check(normalized(input) == nil, "Invalid or sensitive address is rejected")
        }
        check(normalized(String(repeating: "a", count: 2049)) == nil, "Excessively long input rejected before request")

        let systemConfiguration = WebsiteProbe.makeConfiguration(strategy: .system, timeout: 8, protocolClasses: [WebsiteOfflineProtocol.self])
        let directConfiguration = WebsiteProbe.makeConfiguration(strategy: .direct, timeout: 8, protocolClasses: [WebsiteOfflineProtocol.self])
        check(systemConfiguration.connectionProxyDictionary == nil, "System strategy preserves default proxy resolution")
        check(directConfiguration.connectionProxyDictionary?.isEmpty == true, "Direct strategy disables HTTP proxy only for this session")
        check(systemConfiguration.urlCredentialStorage == nil && directConfiguration.urlCredentialStorage == nil, "Neither strategy reads shared saved credentials")
        check(systemConfiguration.httpCookieStorage == nil && !systemConfiguration.httpShouldSetCookies && systemConfiguration.httpCookieAcceptPolicy == .never, "Cookies have no storage or acceptance path")
        check(systemConfiguration.urlCache == nil && systemConfiguration.timeoutIntervalForResource == 8, "Session has no response cache and an eight-second deadline")

        // URLProtocol batches synthetic response callbacks until finish on macOS.
        // Check the production delegate's actual disposition independently, without
        // starting this suspended fixture task or pretending a body was downloaded.
        let fixtureURL = URL(string: "https://fixture.invalid/delegate")!
        let fixtureSession = URLSession(configuration: systemConfiguration)
        let suspendedTask = fixtureSession.dataTask(with: fixtureURL)
        let headerOperation = WebsiteHeaderOperation(url: fixtureURL, strategy: .system, timeout: 0.2)
        let headerResponse = HTTPURLResponse(url: fixtureURL, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: ["Content-Length": "9999999999"])!
        var disposition: URLSession.ResponseDisposition?
        headerOperation.urlSession(fixtureSession, dataTask: suspendedTask, didReceive: headerResponse) { disposition = $0 }
        check(disposition == .cancel, "Production response delegate rejects body delivery at headers")
        let headerResult = await headerOperation.run(configuration: systemConfiguration)
        check(headerResult.state == .accessible, "Stopping at headers preserves the successful HTTP result")

        let redirectCases: [(String, Bool)] = [
            ("https://net.szu.edu.cn/", true),
            ("https://net.szu.edu.cn:443/srun_portal_pc?fixture=private", true),
            ("http://172.30.255.42:801/", true),
            ("http://172.30.255.42:801/eportal?fixture=private", true),
            ("https://ordinary-redirect.invalid/", false),
            ("https://net.szu.edu.cn.evil.invalid/srun_portal_pc", false),
            ("https://net.szu.edu.cn:444/srun_portal_pc", false),
            ("http://net.szu.edu.cn/srun_portal_pc", false),
            ("https://172.30.255.42:801/eportal", false),
            ("http://172.30.255.42/eportal", false),
            ("https://user:pass@net.szu.edu.cn/srun_portal_pc", false),
            ("https://net.szu.edu.cn/cgi-bin/get_challenge", false),
            ("https://net.szu.edu.cn/cgi-bin/srun_portal", false),
            ("http://172.30.255.42:801/eportal/portal/login", false),
            ("https://www.apple.com/library/test/success.html", false),
            ("http://www.msftconnecttest.com/connecttest.txt", false),
            ("https://net.szu.edu.cn/%73run_portal_pc", false),
            ("https://net.szu.edu.cn/srun_portal_pc/extra", false),
            ("https://net.szu.edu.cn/srun_portal_pc?fixture=" + String(repeating: "x", count: 2048), false),
            ("https://net.szu.edu.cn/srun_portal_pc\n", false),
            ("https://net.szu.edu.cn\\@evil.invalid/srun_portal_pc", false)
        ]
        for redirectDelegate in [false, true] {
            for (location, expected) in redirectCases {
                let operation = WebsiteHeaderOperation(url: fixtureURL, strategy: .system, timeout: 0.2)
                let response = HTTPURLResponse(url: fixtureURL, statusCode: 302, httpVersion: "HTTP/1.1",
                                               headerFields: ["Location": location])!
                if redirectDelegate {
                    var followed = true
                    operation.urlSession(fixtureSession, task: suspendedTask, willPerformHTTPRedirection: response,
                                         newRequest: URLRequest(url: fixtureURL)) { followed = $0 != nil }
                    check(!followed, "Redirect callback never follows even an allowed campus target")
                } else {
                    var disposition: URLSession.ResponseDisposition?
                    operation.urlSession(fixtureSession, dataTask: suspendedTask, didReceive: response) { disposition = $0 }
                    check(disposition == .cancel, "Header callback cancels redirect response body")
                }
                let result = await operation.run(configuration: systemConfiguration)
                check((result.recoverySignal == .campusRedirect) == expected,
                      "Both response callbacks apply the exact campus-homepage allowlist")
                check(result.hasHTTPResponse && result.displayURL == fixtureURL.absoluteString && !result.detail.contains("private"),
                      "Only the redirect classification is retained; destination and query are absent")
            }
        }

        let portalBase = URL(string: "https://net.szu.edu.cn/nested/index")!
        for location in ["../srun_portal_pc", "%2e%2e/srun_portal_pc"] {
            let result = WebsiteProbeResult.response(302, url: portalBase, strategy: .direct, elapsed: 0, location: location)
            check(result.recoverySignal == .none, "Relative raw and encoded dot path segments cannot normalize into campus evidence")
        }
        let queryDots = WebsiteProbeResult.response(302, url: portalBase, strategy: .direct, elapsed: 0,
                                                    location: "/srun_portal_pc?note=../ordinary")
        check(queryDots.recoverySignal == .campusRedirect, "Dot text in the query does not alter canonical portal-path recognition")
        let portalPrefix = "https://net.szu.edu.cn/srun_portal_pc?fixture="
        let boundedLocation = portalPrefix + String(repeating: "x", count: 2048 - portalPrefix.utf8.count)
        let boundedResult = WebsiteProbeResult.response(302, url: portalBase, strategy: .direct, elapsed: 0,
                                                        location: boundedLocation)
        check(boundedResult.recoverySignal == .campusRedirect, "A canonical campus Location at the 2048-byte limit remains valid")
        let oversizedResult = WebsiteProbeResult.response(302, url: portalBase, strategy: .direct, elapsed: 0,
                                                          location: boundedLocation + "x")
        check(oversizedResult.recoverySignal == .none, "A canonical campus Location beyond 2048 UTF-8 bytes supplies no recovery signal")

        let emptyLocation = WebsiteProbeResult.response(302, url: URL(string: "https://net.szu.edu.cn/srun_portal_pc")!,
                                                         strategy: .direct, elapsed: 0, location: "")
        check(emptyLocation.recoverySignal == .none, "An empty Location never invents a campus redirect target")

        for sessionLevel in [false, true] {
            for method in [NSURLAuthenticationMethodHTTPBasic, NSURLAuthenticationMethodHTTPDigest, NSURLAuthenticationMethodNTLM, NSURLAuthenticationMethodNegotiate, NSURLAuthenticationMethodClientCertificate] {
                let operation = WebsiteHeaderOperation(url: fixtureURL, strategy: .system, timeout: 0.2)
                let space = URLProtectionSpace(host: "fixture.invalid", port: 443, protocol: "https", realm: "private", authenticationMethod: method)
                let challenge = URLAuthenticationChallenge(protectionSpace: space, proposedCredential: URLCredential(user: "unused", password: "unused", persistence: .none), previousFailureCount: 0, failureResponse: nil, error: nil, sender: UnusedChallengeSender())
                var authDisposition: URLSession.AuthChallengeDisposition?
                var credential: URLCredential?
                if sessionLevel {
                    operation.urlSession(fixtureSession, didReceive: challenge) { authDisposition = $0; credential = $1 }
                } else {
                    operation.urlSession(fixtureSession, task: suspendedTask, didReceive: challenge) { authDisposition = $0; credential = $1 }
                }
                check(authDisposition == .cancelAuthenticationChallenge && credential == nil, "Task and session challenges cannot use proposed or stored credentials")
                let result = await operation.run(configuration: systemConfiguration)
                let expected: WebsiteProbeState = method == NSURLAuthenticationMethodClientCertificate ? .tls : .authenticationRequired
                check(result.state == expected, "Task and session authentication challenges yield a clear diagnostic result")
            }
            let trustOperation = WebsiteHeaderOperation(url: fixtureURL, strategy: .system, timeout: 0.2)
            let trustSpace = URLProtectionSpace(host: "fixture.invalid", port: 443, protocol: "https", realm: nil, authenticationMethod: NSURLAuthenticationMethodServerTrust)
            let trustChallenge = URLAuthenticationChallenge(protectionSpace: trustSpace, proposedCredential: nil, previousFailureCount: 0, failureResponse: nil, error: nil, sender: UnusedChallengeSender())
            var trustDisposition: URLSession.AuthChallengeDisposition?
            var trustCredential: URLCredential?
            if sessionLevel {
                trustOperation.urlSession(fixtureSession, didReceive: trustChallenge) { trustDisposition = $0; trustCredential = $1 }
            } else {
                trustOperation.urlSession(fixtureSession, task: suspendedTask, didReceive: trustChallenge) { trustDisposition = $0; trustCredential = $1 }
            }
            check(trustCredential == nil, "Task and session TLS callbacks never supply a trust-bypass credential")
            check(trustDisposition == .performDefaultHandling, "Task and session TLS callbacks keep normal platform certificate validation")
        }
        fixtureSession.invalidateAndCancel()

        let probe = WebsiteProbe(protocolClasses: [WebsiteOfflineProtocol.self], timeout: 0.2)
        WebsiteOfflineProtocol.reset(.hang)
        let invalid = await probe.probe("https://user:SECRET@example.invalid/?token=SECRET")
        check(invalid.state == .invalidInput && invalid.displayURL == nil, "Invalid result contains no original URL")
        check(!invalid.detail.contains("SECRET") && WebsiteOfflineProtocol.snapshot().requests.isEmpty, "Sensitive input never creates a request or leaks through error text")

        for (code, state) in [(200, WebsiteProbeState.accessible), (204, .accessible), (302, .redirected), (401, .authenticationRequired), (403, .httpStatus), (404, .httpStatus), (500, .httpStatus)] {
            WebsiteOfflineProtocol.reset(.headers(code))
            let result = await probe.probe("https://fixture.invalid/test")
            check(result.state == state && result.statusCode == code && result.hasHTTPResponse,
                  "HTTP status classified accurately as an actual response")
            check(result.recoverySignal == .none, "A business HTTP response alone requests no campus recovery")
            check(result.elapsed < 0.2, "Header-only result completes before the deadline")
            try await WebsiteOfflineProtocol.idle()
            check(WebsiteOfflineProtocol.snapshot().stopped == 1, "Header response cancels protocol lifecycle")
        }
        WebsiteOfflineProtocol.reset(.redirect)
        let redirected = await probe.probe("https://fixture.invalid/redirect", strategy: .direct)
        try await WebsiteOfflineProtocol.idle()
        check(redirected.state == .redirected && redirected.statusCode == 302, "Transport redirect is reported without following")
        check(WebsiteOfflineProtocol.snapshot().requests.count == 1, "Redirect destination is never requested")
        check(!(redirected.detail + (redirected.displayURL ?? "")).contains("secret"), "Redirect location tokens never enter result")
        check(redirected.strategy == .direct, "Result retains chosen proxy strategy")

        for (code, state) in [(URLError.cannotFindHost, WebsiteProbeState.dns), (.dnsLookupFailed, .dns), (.secureConnectionFailed, .tls), (.serverCertificateUntrusted, .tls), (.timedOut, .timeout), (.cancelled, .cancelled), (.notConnectedToInternet, .network), (.cannotConnectToHost, .network), (.networkConnectionLost, .network),
                              (.badURL, .network), (.unsupportedURL, .network), (.badServerResponse, .network), (.userAuthenticationRequired, .authenticationRequired), (.appTransportSecurityRequiresSecureConnection, .securityPolicy)] {
            WebsiteOfflineProtocol.reset(.error(code))
            let result = await probe.probe("http://fixture.invalid/errors")
            try await WebsiteOfflineProtocol.idle()
            check(result.state == state && result.statusCode == nil && !result.hasHTTPResponse,
                  "Transport error classified without inventing HTTP status")
            let connectionErrors: Set<URLError.Code> = [.timedOut, .cannotFindHost, .dnsLookupFailed,
                .cannotConnectToHost, .networkConnectionLost, .notConnectedToInternet]
            check((result.recoverySignal == .connectionFailure) == connectionErrors.contains(code),
                  "Only the explicit connection-error whitelist requests recovery")
            check(!result.detail.contains("SECRET"), "Raw error description cannot leak URL tokens")
            if state == .securityPolicy {
                check(result.title == "需要 HTTPS 网址" && result.detail.contains("系统安全策略") && result.detail.contains("不能据此判断网站断网"), "ATS rejection explains the local security restriction without declaring the website offline")
            }
        }

        WebsiteOfflineProtocol.reset(.cookie)
        let cookie = await probe.probe("https://fixture.invalid/cookie")
        try await WebsiteOfflineProtocol.idle()
        check(cookie.state == .accessible, "Cookie-bearing response can be inspected")
        WebsiteOfflineProtocol.reset(.headers(200))
        let next = await probe.probe("https://fixture.invalid/next")
        try await WebsiteOfflineProtocol.idle()
        let request = WebsiteOfflineProtocol.snapshot().requests.first!
        check(next.state == .accessible && request.value(forHTTPHeaderField: "Cookie") == nil, "Response cookies never follow subsequent checks")
        check(request.value(forHTTPHeaderField: "Authorization") == nil && request.value(forHTTPHeaderField: "Proxy-Authorization") == nil, "Request includes no credential headers")
        check(!request.httpShouldHandleCookies, "Request opts out of ambient cookies")
        check(request.cachePolicy == .reloadIgnoringLocalCacheData, "Diagnostic bypasses response cache")
        check(next.strategy == .system, "Default strategy follows system configuration")

        WebsiteOfflineProtocol.reset(.hang)
        let timeoutStart = ProcessInfo.processInfo.systemUptime
        let timedOut = await probe.probe("https://fixture.invalid/hang")
        try await WebsiteOfflineProtocol.idle()
        check(timedOut.state == .timeout, "Own deadline handles stalled URLProtocol without relying on URLSession timeout")
        check(ProcessInfo.processInfo.systemUptime - timeoutStart < 0.6, "Deadline is bounded")
        check(WebsiteOfflineProtocol.snapshot().stopped == 1, "Deadline releases active request")

        WebsiteOfflineProtocol.reset(.hang)
        let running = Task { await probe.probe("https://fixture.invalid/cancel") }
        for _ in 0..<100 {
            if WebsiteOfflineProtocol.snapshot().active == 1 { break }
            try await Task.sleep(for: .milliseconds(1))
        }
        running.cancel()
        let cancelled = await running.value
        try await WebsiteOfflineProtocol.idle()
        check(cancelled.state == .cancelled, "Caller cancellation returns cancelled result")
        check(WebsiteOfflineProtocol.snapshot().stopped == 1, "Caller cancellation releases underlying request")

        WebsiteOfflineProtocol.reset(.hang)
        let beforeStart = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return await probe.probe("https://fixture.invalid/pre-cancel")
        }
        let preCancelled = await beforeStart.value
        try await WebsiteOfflineProtocol.idle()
        check(preCancelled.state == .cancelled && WebsiteOfflineProtocol.snapshot().requests.isEmpty, "Cancellation before start sends no request")
        check(WebsiteProbeStrategy.allCases.count == 2 && WebsiteProbeStrategy.direct.detail.contains("TUN"), "Strategy explains TUN limitation")
        print("Website diagnostics offline assertions: \(passed)")
    }
}
