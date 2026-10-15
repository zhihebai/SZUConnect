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
