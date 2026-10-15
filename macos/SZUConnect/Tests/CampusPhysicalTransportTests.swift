import Foundation
import Darwin

private final class FixtureState: @unchecked Sendable {
    private let lock = NSLock()
    private var cookies: [String] = []
    private var starts = 0
    private var current = true
    let release = DispatchSemaphore(value: 0)
    func record(_ cookie: String) -> Int { lock.lock(); defer { lock.unlock() }; cookies.append(cookie); starts += 1; return starts }
    var count: Int { lock.lock(); defer { lock.unlock() }; return starts }
    var recordedCookies: [String] { lock.lock(); defer { lock.unlock() }; return cookies }
    var routeCurrent: Bool { lock.lock(); defer { lock.unlock() }; return current }
    func changeRoute() { lock.lock(); current = false; lock.unlock() }
}

@main
private enum CampusPhysicalTransportTests {
    static var assertions = 0
    static func expect(_ value: @autoclosure () -> Bool, _ label: String) {
        assertions += 1
        precondition(value(), label)
    }
    static func expectError(_ error: CampusPhysicalTransportError, label: String, _ block: () async throws -> Void) async {
        do { try await block(); expect(false, label) }
        catch let actual as CampusPhysicalTransportError { expect(actual == error, label) }
        catch { expect(false, label) }
    }
    static let route = CampusPhysicalRoute(interfaceName: "en0", interfaceIndex: 1, addresses: ["192.0.2.2"], dnsServers: ["192.0.2.53"], identity: "offline")
    static let page = URL(string: "https://net.szu.edu.cn/srun_portal_pc")!
    static let resolver: CampusPhysicalTransport.Resolver = { _, _, _, cancelled in
        if cancelled() { throw CampusPhysicalDNSError.cancelled }
        return ["192.0.2.10"]
    }
    static func nativeValid(_ text: String, addresses: String = "192.0.2.10", cookie: String = "", index: UInt32 = 1,
                            timeout: Double = 1, limit: Int = 100) -> Bool {
        text.withCString { url in addresses.withCString { resolved in cookie.withCString { cookie in "192.0.2.2".withCString { source in "en0".withCString { interfaceName in
            var request = SZUCampusRequest(url: url, resolved_addresses: resolved, cookie: cookie, source_ipv4: source, interface_name: interfaceName, route_is_current: { _ in 1 }, route_context: nil, interface_index: index,
                                           timeout_seconds: timeout, response_limit: limit)
            return szu_campus_request_is_valid(&request) != 0
        } } } } }
    }
    static func headers(_ text: String, status: Int = 200, limit: Int = 100) -> Bool {
        (try? CampusPhysicalTransport.parseHeaders(text, status: status, limit: limit)) != nil
    }
    static func main() async throws {
        let allowed = ["https://net.szu.edu.cn/", "https://net.szu.edu.cn/srun_portal_pc", "https://net.szu.edu.cn:443/cgi-bin/get_challenge?username=offline", "https://net.szu.edu.cn/cgi-bin/srun_portal?x=offline", "http://172.30.255.42:801/", "http://172.30.255.42:801/eportal", "http://172.30.255.42:801/eportal/portal/login?offline=1", "https://www.apple.com/library/test/success.html", "http://www.msftconnecttest.com/connecttest.txt"]
        for text in allowed {
            expect(CampusPhysicalTransport.isAllowed(URL(string: text)!), "Swift fixed endpoint accepted")
            expect(nativeValid(text, addresses: text.contains("172.30.255.42") ? "" : "192.0.2.10"), "native fixed endpoint accepted")
        }
        let forbidden = ["https://example.com/", "https://net.szu.edu.cn.evil.invalid/srun_portal_pc", "http://net.szu.edu.cn/srun_portal_pc", "https://net.szu.edu.cn:444/srun_portal_pc", "https://net.szu.edu.cn/srun_portal_pc?x=1", "https://net.szu.edu.cn/srun_portal_pc#x", "https://u:p@net.szu.edu.cn/srun_portal_pc", "https://net.szu.edu.cn/unknown", "https://www.apple.com/", "http://172.30.255.42:801/eportal#x", "http://172.30.255.42/eportal", "http://www.msftconnecttest.com/connecttest.txt?x=1", "https://127.0.0.1/srun_portal_pc", "file:///srun_portal_pc"]
        for text in forbidden {
            expect(!CampusPhysicalTransport.isAllowed(URL(string: text)!), "Swift unknown endpoint rejected")
            expect(!nativeValid(text), "native unknown endpoint rejected")
        }
        for answers in ["", "localhost", "192.0.2.10,", "192.0.2.10:80", "192.0.2.10\n", "::1", "192.0.2.10,invalid"] {
            expect(!nativeValid(page.absoluteString, addresses: answers), "untrusted resolver syntax rejected")
        }
        expect(nativeValid(page.absoluteString, addresses: "192.0.2.10,192.0.2.11"), "multiple verified IPv4 answers supported")
        expect(!nativeValid(page.absoluteString, index: 0), "no unbound socket allowed")
        expect(!nativeValid(page.absoluteString, timeout: 0), "no zero timeout")
        expect(!nativeValid(page.absoluteString, timeout: 13), "bounded native timeout")
        expect(!nativeValid(page.absoluteString, timeout: .nan), "invalid native timeout")
        expect(!nativeValid(page.absoluteString, limit: 0), "positive body bound")
        expect(!nativeValid(page.absoluteString, limit: 1_048_577), "native body bound ceiling")
        expect(!nativeValid(page.absoluteString, cookie: "a=1\r\nInjected: x"), "cookie header injection rejected")
        expect(szu_campus_test_bounds() == 1, "actual native body/header/cancellation callbacks enforce bounds")
        expect(szu_campus_test_security_options() == 1, "native strict TLS, proxy and redirect options configure successfully")
        let loopback = if_nametoindex("lo0")
        expect(loopback > 0 && szu_campus_test_bound_socket(loopback) == 1, "actual socket binds exact source plus interface; prereq rejects wrong source, stale epoch and cancellation")
        let cancellation = try CampusPhysicalCancellation()
        expect(!cancellation.isCancelled, "native cancellation begins clear")
        cancellation.cancel(); expect(cancellation.isCancelled, "native cancellation atomic flag")
        let cancelledResult = page.absoluteString.withCString { text in "192.0.2.10".withCString { address in "192.0.2.2".withCString { source in "en0".withCString { interfaceName in
            var request = SZUCampusRequest(url: text, resolved_addresses: address, cookie: nil, source_ipv4: source, interface_name: interfaceName, route_is_current: { _ in 1 }, route_context: nil, interface_index: 1,
                                           timeout_seconds: 1, response_limit: 100)
            return szu_campus_request(&request, cancellation.pointer)
        } } } }
        expect(cancelledResult.error == SZU_CAMPUS_CANCELLED && cancelledResult.body == nil, "cancelled native transfer opens no request")

        let parsed = try CampusPhysicalTransport.parseHeaders("HTTP/2 200\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nX-Test: x\r\nx-test: y\r\n\r\nSet-Cookie: trailer=blocked\r\n", status: 200, limit: 100)
        expect(parsed.cookies == ["a=1", "b=2"], "duplicate cookies kept and trailers excluded")
        expect(parsed.fields["X-Test"] == "x, y", "case-insensitive duplicate headers combined")
        expect(!headers("HTTP/1.1 201 Created\r\n\r\n"), "response status agrees with native status")
        expect(!headers("HTTP/1.1 200 OK\r\n folded: value\r\n\r\n"), "obsolete folded headers fail closed")
        expect(!headers("HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n"), "invalid header token rejected")
        expect(!headers("HTTP/1.1 200 OK\r\nX-Test: x\u{01}y\r\n\r\n"), "control characters in header values rejected")
        expect(!headers("HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n"), "announced body bound respected")
        expect(!headers("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"), "invalid content length rejected")
        expect(!headers("HTTP/1.1 200 OK\r\nX: " + String(repeating: "x", count: 65_536)), "header byte ceiling")

        var jar = CampusPhysicalCookieJar()
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        jar.update(lines: ["session=one; Path=/; Secure; HttpOnly", "local=two; Path=/cgi-bin", "foreign=blocked; Domain=szu.edu.cn; Path=/", "evil=blocked; Domain=evil.invalid; Path=/"], for: page, now: now)
        expect(jar.header(for: page, now: now) == "session=one", "exact-origin secure cookie accepted")
        let api = URL(string: "https://net.szu.edu.cn/cgi-bin/get_challenge")!
        expect(jar.header(for: api, now: now) == "local=two; session=one", "longest matching cookie path first")
        expect(jar.header(for: URL(string: "https://net.szu.edu.cn/cgi-binary")!, now: now) == "session=one", "cookie path requires slash boundary")
        expect(jar.header(for: URL(string: "http://net.szu.edu.cn/")!, now: now).isEmpty, "secure cookie cannot downgrade")
        expect(jar.header(for: URL(string: "https://www.apple.com/")!, now: now).isEmpty, "cookies cannot enter other probe host")
        jar.update(lines: ["session=gone; Path=/; Max-Age=0"], for: page, now: now)
        expect(jar.header(for: page, now: now).isEmpty, "expiry removes cookie")
        jar.update(lines: (0..<100).map { "n\($0)=v; Path=/" }, for: page, now: now)
        expect(jar.header(for: page, now: now).split(separator: ";").count <= 64, "cookie retained count bounded")
        jar.removeAll(); expect(jar.header(for: api, now: now).isEmpty, "reset discards cookie RAM")

        let fixture = FixtureState()
        let transport = CampusPhysicalTransport(route: route, cookies: true, routeIsCurrent: { _ in true }, resolve: resolver,
            transfer: { _, _, _, cookie, _, _, _, _ in
                let count = fixture.record(cookie)
                return CampusPhysicalTransferResult(data: Data("page".utf8), headers: "HTTP/1.1 200 OK\r\nSet-Cookie: s=\(count); Path=/; Secure\r\n\r\n", status: 200)
            })
        let first = try await transport.data(for: URLRequest(url: page), timeout: 1, limit: 100)
        expect(first.0 == Data("page".utf8) && first.1.statusCode == 200, "bounded response reaches actor")
        _ = try await transport.data(for: URLRequest(url: api), timeout: 1, limit: 100)
        expect(fixture.recordedCookies == ["", "s=1"], "authentication request cookie chain ephemeral")
        await transport.cancel()
        _ = try await transport.data(for: URLRequest(url: page), timeout: 1, limit: 100)
        expect(fixture.recordedCookies.last == "", "cancel clears cookie chain")
        let probeFixture = FixtureState()
        let probes = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in true }, resolve: resolver,
            transfer: { _, _, _, cookie, _, _, _, _ in
                _ = probeFixture.record(cookie)
                return CampusPhysicalTransferResult(data: Data(), headers: "HTTP/1.1 200 OK\r\nSet-Cookie: s=ignored; Path=/\r\n\r\n", status: 200)
            })
        _ = try await probes.data(for: URLRequest(url: page), timeout: 1, limit: 100)
        _ = try await probes.data(for: URLRequest(url: page), timeout: 1, limit: 100)
        expect(probeFixture.recordedCookies == ["", ""], "probe transport neither retains nor sends cookies")
        await expectError(.response, label: "unknown URL never reaches DNS or native transfer") {
            _ = try await transport.data(for: URLRequest(url: URL(string: "https://example.com/")!), timeout: 1, limit: 100)
        }
        var post = URLRequest(url: page); post.httpMethod = "POST"
        let invalidPost = post
        await expectError(.response, label: "unsupported request body/method rejected") { _ = try await transport.data(for: invalidPost, timeout: 1, limit: 100) }
        let routeFixture = FixtureState()
        let changing = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in routeFixture.routeCurrent },
            resolve: { _, _, _, _ in routeFixture.changeRoute(); return ["192.0.2.10"] },
            transfer: { _, _, _, _, _, _, _, _ in preconditionFailure("changed route must not send") })
        await expectError(.cancelled, label: "route change after DNS stops before socket") { _ = try await changing.data(for: URLRequest(url: page), timeout: 1, limit: 100) }
        for (dnsError, expected) in [(CampusPhysicalDNSError.timeout, CampusPhysicalTransportError.timeout), (.unavailable, .dns), (.invalidResponse, .dns), (.cancelled, .cancelled)] {
            let failing = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in true }, resolve: { _, _, _, _ in throw dnsError })
            await expectError(expected, label: "DNS failure retains distinct error") { _ = try await failing.data(for: URLRequest(url: page), timeout: 1, limit: 100) }
        }
        let delayed = FixtureState()
        let session = CampusPhysicalTransport(route: route, cookies: true, routeIsCurrent: { _ in true }, resolve: resolver,
            transfer: { _, _, _, cookie, _, _, _, _ in
                let count = delayed.record(cookie)
                if count == 1 { _ = delayed.release.wait(timeout: .now() + 2) }
                return CampusPhysicalTransferResult(data: Data(), headers: "HTTP/1.1 200 OK\r\nSet-Cookie: late=blocked; Path=/; Secure\r\n\r\n", status: 200)
            })
        let task = Task { try await session.data(for: URLRequest(url: page), timeout: 1, limit: 100) }
        for _ in 0..<100 where delayed.count == 0 { try await Task.sleep(for: .milliseconds(2)) }
        expect(delayed.count == 1, "background transfer began")
        await session.cancel(); delayed.release.signal()
        await expectError(.cancelled, label: "cancelled completion cannot publish response") { _ = try await task.value }
        _ = try await session.data(for: URLRequest(url: page), timeout: 1, limit: 100)
        expect(delayed.recordedCookies == ["", ""], "late cancelled response cannot restore cookies")
        let taskCancelled = FixtureState()
        let pending = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in true }, resolve: resolver,
            transfer: { _, _, _, _, _, _, flag, _ in
                _ = taskCancelled.record("")
                for _ in 0..<1000 where !flag.isCancelled { usleep(1_000) }
                if flag.isCancelled { throw CampusPhysicalTransportError.cancelled }
                throw CampusPhysicalTransportError.timeout
            })
        let requestTask = Task { try await pending.data(for: URLRequest(url: page), timeout: 1, limit: 100) }
        for _ in 0..<100 where taskCancelled.count == 0 { try await Task.sleep(for: .milliseconds(2)) }
        requestTask.cancel()
        await expectError(.cancelled, label: "Task cancellation reaches active native worker flag") { _ = try await requestTask.value }
        print("CampusPhysicalTransport: \(assertions) assertions passed (offline; no auth, DNS or TCP connect).")
    }
}
