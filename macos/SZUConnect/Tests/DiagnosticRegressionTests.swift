import Foundation

private final class HeadersFixture: URLProtocol {
    static var responseCode = 200
    static var requestCount = 0
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.requestCount += 1
        precondition(request.httpMethod == "GET")
        precondition(request.value(forHTTPHeaderField: "Authorization") == nil)
        precondition(request.value(forHTTPHeaderField: "Cookie") == nil)
        let response = HTTPURLResponse(url: request.url!, statusCode: Self.responseCode,
            httpVersion: "HTTP/1.1", headerFields: ["Content-Length": "900000000"])!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        // URLProtocol buffers synthetic headers until finish on macOS; no body is supplied.
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}

@main @MainActor enum DiagnosticRegressionTests {
    static var passed = 0
    static func check(_ condition: Bool, _ name: String) {
        precondition(condition, name); passed += 1
    }
    static func main() async throws {
        let probe = WebsiteProbe(protocolClasses: [HeadersFixture.self], timeout: 0.2)
        let url = URL(string: "https://fixture.invalid/")!
        for (code, expected) in [(200,"reachable"),(204,"reachable"),(302,"redirect"),(401,"authentication"),(403,"httpStatus"),(500,"httpStatus")] {
            HeadersFixture.responseCode = code
            let token = await HeaderCheck.token(url, followSystemProxy: false, checker: probe)
            check(token == expected, "HTTP \(code) is classified without waiting for body")
        }
        check(HeadersFixture.requestCount == 6, "Exactly one request per check; no redirects or retries")
        let configuration = WebsiteProbe.makeConfiguration(strategy: .direct, timeout: 8, protocolClasses: nil)
        check(configuration.urlCredentialStorage == nil && configuration.httpCookieStorage == nil,
              "Compatibility check reuses credential-free configuration")
        check(configuration.connectionProxyDictionary?.isEmpty == true, "Direct strategy ignores only system proxy")
        check(HeaderCheck.resultToken(.tls) == "tls" && HeaderCheck.resultToken(.cancelled) == "cancelled",
              "TLS and cancellation are not reachable")
        check(HeaderCheck.resultToken(.securityPolicy) == "securityPolicy", "ATS limitation is preserved")
        let pureTUN = await HeaderCheck.compatibility(proxy: "off", tunnel: "detected",
            physicalCheck: { _ in "timeout" }, systemCheck: { _ in "reachable" })
        check(pureTUN.proxy == "off" && pureTUN.campusSystem == "reachable" && pureTUN.publicSystem == "reachable",
              "Pure TUN still checks both system endpoints without misreporting system proxy enabled")
        check(pureTUN.campusDirect == "timeout" && pureTUN.publicDirect == "timeout",
              "Physical route diagnosis retains its independent result")
        var activeChecks = 0
        var cancelResume: CheckedContinuation<CompatInput, Never>?
        let cancellable = CompatibilityRunner(check: { _, _ in
            activeChecks += 1
            return await withCheckedContinuation { cancelResume = $0 }
        })
        cancellable.run(proxy: "off", tunnel: "detected")
        while cancelResume == nil { await Task.yield() }
        cancellable.run(proxy: "on", tunnel: "notDetected")
        check(activeChecks == 1 && cancellable.running,
              "Repeated activation during diagnosis keeps one running operation")
        cancellable.cancel()
        cancelResume?.resume(returning: CompatInput(proxy: "off", tunnel: "detected",
            campusDirect: "reachable", campusSystem: "reachable", publicDirect: "reachable", publicSystem: "reachable"))
        while cancellable.running { await Task.yield() }
        check(cancellable.input?.campusDirect == "cancelled" && cancellable.verdict?.verdict == "cancelled",
              "A cancelled tool cannot publish a successful verdict when a non-cooperative check returns")
        check(cancellable.input?.proxy == "off" && cancellable.input?.tunnel == "detected",
              "A rejected repeated activation cannot replace the active operation's environment")
        var resume: CheckedContinuation<CompatInput, Never>?
        let runner = CompatibilityRunner(check: { _, _ in await withCheckedContinuation { resume = $0 } })
        runner.run(proxy: "on", tunnel: "detected")
        while resume == nil { await Task.yield() }
        runner.reset()
        resume?.resume(returning: CompatInput(proxy: "on", tunnel: "detected", campusDirect: "reachable", publicDirect: "reachable"))
        for _ in 0..<10 { await Task.yield() }
        check(!runner.running && runner.input == nil && runner.verdict == nil, "Late result cannot resurrect a closed or changed-network check")
        let preview = CompatibilityRunner(check: { _, _ in fatalError("Preview must not access transport") })
        preview.run(proxy: "unknown", tunnel: "unknown", preview: true)
        while preview.running { await Task.yield() }
        check(preview.verdict == nil && preview.input?.campusDirect == "notRun", "Preview never invents a real diagnostic pass")
        print("\(passed) diagnostic regression assertions passed; fake transport only.")
    }
}
