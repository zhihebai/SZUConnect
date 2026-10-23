#if os(macOS)
import Foundation

private final class Clock: @unchecked Sendable {
    private let lock = NSLock()
    private var value: TimeInterval = 10
    var now: TimeInterval { lock.lock(); defer { lock.unlock() }; return value }
    func set(_ next: TimeInterval) { lock.lock(); value = next; lock.unlock() }
}
private final class State: @unchecked Sendable {
    var associated = 0, encrypted = 0, transfer = 0
    var current = true, cancelled = false
}
/// Wall-clock timeouts only prevent a broken fixture from hanging the suite.
/// Resolver deadlines and request ordering are controlled by the clock and gates.
private final class ResolutionGate: @unchecked Sendable {
    private let entered = DispatchSemaphore(value: 0), finish = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var receivedBudget: TimeInterval?
    var budget: TimeInterval? { lock.lock(); defer { lock.unlock() }; return receivedBudget }
    func block(budget: TimeInterval) throws {
        lock.lock(); receivedBudget = budget; lock.unlock()
        entered.signal()
        guard finish.wait(timeout: .now() + 5) == .success else { throw CampusPhysicalTransportError.timeout }
    }
    func waitUntilEntered() async -> Bool {
        await withCheckedContinuation { continuation in
            DispatchQueue.global().async { continuation.resume(returning: self.entered.wait(timeout: .now() + 5) == .success) }
        }
    }
    func release() { finish.signal() }
}
private final class CommitDeadlineClock: @unchecked Sendable {
    private let lock = NSLock()
    private var value: TimeInterval = 10
    private var readsBeforeExpiration: Int?
    var now: TimeInterval {
        lock.lock(); defer { lock.unlock() }
        if let remaining = readsBeforeExpiration {
            if remaining == 0 { value = 12.01; readsBeforeExpiration = nil }
            else { readsBeforeExpiration = remaining - 1 }
        }
        return value
    }
    func expireAfterReplyAcceptance() {
        lock.lock(); value = 11.99; readsBeforeExpiration = 1; lock.unlock()
    }
}
@main struct BoundedResolutionTests {
    static var assertions = 0
    static func expect(_ value: Bool, _ label: String) {
        assertions += 1
        guard value else { fatalError("FAIL: \(label)") }
    }
    static func fails(_ expected: CampusPhysicalTransportError, _ label: String, _ action: () throws -> Void) {
        do { try action(); fatalError("FAIL: \(label) did not fail") }
        catch let error as CampusPhysicalTransportError { expect(error == expected, label) }
        catch { fatalError("FAIL: \(label): unexpected error") }
    }
    static let host = "net.szu.edu.cn"
    static let route = CampusPhysicalRoute(interfaceName: "en99", interfaceIndex: 99,
        addresses: ["192.0.2.7"], dnsServers: ["192.0.2.53"], identity: "fake-route", serviceID: "fake-service")
    static let values = ["192.0.2.80"]
    static func packet(host: String = host, ttl: UInt32 = 60, addresses: [[UInt8]] = [[192, 0, 2, 80]], opt: Bool = false) -> Data {
        var bytes = [UInt8](try! CampusPhysicalDNS.query(host: host, transaction: 0x1234))
        bytes[2] = 0x81; bytes[3] = 0x80; bytes[7] = UInt8(addresses.count); bytes[11] = opt ? 1 : 0
        for address in addresses {
            bytes += [0xc0, 0x0c, 0, 1, 0, 1, UInt8(ttl >> 24), UInt8((ttl >> 16) & 255), UInt8((ttl >> 8) & 255), UInt8(ttl & 255), 0, 4] + address
        }
        // Class 1 is deliberately unusual: even then, OPT's flag field must be ignored.
        if opt { bytes += [0, 0, 41, 0, 1, 0, 0, 0, 0, 0, 0] }
        return Data(bytes)
    }
    static func raw(_ data: Data = packet(), headers: String = "", status: Int = 200) -> CampusPhysicalTransferResult {
        .init(data: data, headers: "HTTP/1.1 \(status) OK\r\nContent-Type: application/dns-message\r\n" + headers + "\r\n", status: status)
    }
    static func main() async throws {
        let clock = Clock(), state = State()
        let cache = CampusPhysicalAddressResolver(now: { clock.now })
        func lookup(_ name: String = host, route: CampusPhysicalRoute = route,
                    ttl: TimeInterval = 60, answer: [String] = values,
                    associated: CampusPhysicalTransportError? = .dns,
                    encrypted: CampusPhysicalTransportError? = nil) throws -> [String] {
            try cache.resolve(host: name, route: route, timeout: 2,
                cancelled: { state.cancelled }, current: { state.current },
                associated: { _, _, budget, _ in
                    state.associated += 1; expect(budget > 0 && budget <= 1, "associated bounded budget")
                    if let associated { throw associated }; return values
                }, encrypted: { _, _, budget in
                    state.encrypted += 1; expect(budget > 0 && budget <= 2, "encrypted bounded budget")
                    if let encrypted { throw encrypted }; return .init(addresses: answer, ttl: ttl)
                })
        }
        expect(try lookup(associated: nil) == values, "associated resolver remains first")
        expect(state.encrypted == 0 && cache.count == 0, "associated answers do not populate fallback cache")
        expect(try lookup() == values && cache.count == 1, "DoH answer remembered")
        let before = state.encrypted
        clock.set(40)
        expect(try lookup(encrypted: .network) == values && state.encrypted == before, "fresh candidate after associated failure")
        expect(try lookup(associated: nil, encrypted: .network) == values && state.encrypted == before, "fresh cache never skips associated DNS")
        clock.set(71)
        expect(try lookup(encrypted: .network) == values, "fixed HTTPS stale candidate after both resolvers fail")
        fails(.tls, "associated security terminates before cache") { _ = try lookup(associated: .tls) }
        fails(.tls, "DoH TLS terminates before stale fallback") { _ = try lookup(encrypted: .tls) }
        state.cancelled = true
        let noRequests = state.associated
        fails(.cancelled, "cancelled never consults cache") { _ = try lookup() }
        expect(state.associated == noRequests, "cancelled opens no new stage")
        state.cancelled = false; state.current = false
        fails(.cancelled, "binding invalidation terminates") { _ = try lookup() }
        state.current = true
        clock.set(86_409)
        expect(try lookup(encrypted: .dns) == values, "24h retention is from original receipt")
        clock.set(86_410)
        fails(.dns, "exact 24h old candidate removed; reads never extend lifetime") { _ = try lookup(encrypted: .dns) }
        expect(cache.count == 0, "expired entries pruned")
        clock.set(90_000)
        _ = try lookup("www.msftconnecttest.com", ttl: 1)
        clock.set(90_002)
        fails(.dns, "HTTP never uses expired candidate") { _ = try lookup("www.msftconnecttest.com", encrypted: .network) }
        expect(cache.count == 0, "HTTP retention equals TTL")
        for ttl in [0.0, -1, .nan, .infinity] {
            cache.clear(); _ = try lookup(ttl: 1)
            clock.set(clock.now + 2)
            expect(try lookup(ttl: ttl, answer: ["192.0.2.81"]) == ["192.0.2.81"], "zero or invalid TTL valid answer usable once")
            expect(cache.count == 0, "zero/invalid TTL actively removes preceding entry")
            fails(.dns, "zero TTL cannot resurrect old addresses") { _ = try lookup(encrypted: .dns) }
        }
        cache.clear(); _ = try lookup(ttl: 20_000)
        clock.set(clock.now + 301)
        let calls = state.encrypted
        _ = try lookup(encrypted: .network)
        expect(state.encrypted == calls + 1, "fresh TTL capped at five minutes")
        cache.clear(); _ = try lookup(ttl: 1); clock.set(clock.now + 2)
        var routes: [CampusPhysicalRoute] = []
        routes.append(.init(interfaceName: "en98", interfaceIndex: 99, addresses: route.addresses, dnsServers: route.dnsServers, identity: route.identity, serviceID: route.serviceID))
        routes.append(.init(interfaceName: route.interfaceName, interfaceIndex: 98, addresses: route.addresses, dnsServers: route.dnsServers, identity: route.identity, serviceID: route.serviceID))
        routes.append(.init(interfaceName: route.interfaceName, interfaceIndex: 99, addresses: ["192.0.2.8"], dnsServers: route.dnsServers, identity: route.identity, serviceID: route.serviceID))
        routes.append(.init(interfaceName: route.interfaceName, interfaceIndex: 99, addresses: route.addresses, dnsServers: ["192.0.2.54"], identity: route.identity, serviceID: route.serviceID))
        routes.append(.init(interfaceName: route.interfaceName, interfaceIndex: 99, addresses: route.addresses, dnsServers: route.dnsServers, identity: "another-attachment", serviceID: route.serviceID))
        routes.append(.init(interfaceName: route.interfaceName, interfaceIndex: 99, addresses: route.addresses, dnsServers: route.dnsServers, identity: route.identity, serviceID: "another-service"))
        for changed in routes { fails(.dns, "complete physical route isolates cached address") { _ = try lookup(route: changed, encrypted: .dns) } }
        cache.clear()
        for index in 0..<30 {
            let unique = CampusPhysicalRoute(interfaceName: "en99", interfaceIndex: 99, addresses: route.addresses, dnsServers: route.dnsServers, identity: "fixture-\(index)")
            _ = try lookup(route: unique)
        }
        expect(cache.count == 24, "cache maximum 24 entries")
        cache.clear()
        fails(.dns, "17 answers refused") { _ = try lookup(answer: Array(repeating: "192.0.2.80", count: 17)) }
        fails(.dns, "FakeIP refused") { _ = try lookup(answer: ["198.18.0.1"]) }
        fails(.dns, "non-whitelisted hostname refused") { _ = try lookup("custom.invalid") }
        for invalid in ["0.0.0.0", "127.0.0.1", "169.254.1.1", "224.0.0.1", "192.000.2.1", "198.19.0.1"] {
            fails(.dns, "invalid returned address refused") { _ = try lookup(answer: [invalid]) }
        }
        let answer = try CampusPhysicalAddressResolver.httpsAnswer(raw(packet(opt: true)), host: host, transaction: 0x1234)
        expect(answer.ttl == 60 && answer.addresses == values, "OPT not mistaken for zero TTL")
        expect(try CampusPhysicalDNS.responseTTL(packet(ttl: 20_000), host: host, transaction: 0x1234) == 20_000,
               "DNS parser preserves lifetime before HTTP Age is deducted")
        for (age, remaining) in [(100, 300.0), (400, 200), (600, 0)] {
            expect(try CampusPhysicalAddressResolver.httpsAnswer(raw(packet(ttl: 600), headers: "Age: \(age)\r\n"),
                host: host, transaction: 0x1234).ttl == remaining,
                "HTTP Age \(age) is subtracted from original TTL 600 before the five-minute cache ceiling")
        }
        for (header, ttl) in [("Age: 10\r\n", 50.0), ("Age: 60\r\n", 0), ("Age: 0\r\nAge: 0\r\n", 0),
                              ("Age: invalid\r\n", 0), ("Age: -1\r\n", 0), ("Age: 18446744073709551616\r\n", 0),
                              ("Age: 00001\r\n", 59), ("aGe:\t59 \t\r\n", 1)] {
            expect(try CampusPhysicalAddressResolver.httpsAnswer(raw(headers: header), host: host, transaction: 0x1234).ttl == ttl, "HTTP Age strictly subtracts or clears TTL")
        }
        for bad in [raw(headers: "Content-Type: text/plain\r\n"), raw(headers: "Content-Encoding: gzip\r\n"),
                    raw(headers: "Content-Type: application/dns-message; charset=utf-8\r\n"), raw(status: 302)] {
            fails(.dns, "DoH response strict media/status") { _ = try CampusPhysicalAddressResolver.httpsAnswer(bad, host: host, transaction: 0x1234) }
        }
        for bytes in [packet(addresses: [[198, 18, 0, 1]]), packet().dropLast(), packet(addresses: (1...17).map { [192, 0, 2, UInt8($0)] })] {
            do { _ = try CampusPhysicalAddressResolver.httpsAnswer(raw(Data(bytes)), host: host, transaction: 0x1234); fatalError("bad DNS accepted") }
            catch { expect(true, "bad DNS packets rejected") }
        }
        let deadlineClock = Clock(), deadlineCache = CampusPhysicalAddressResolver(now: { deadlineClock.now })
        fails(.timeout, "shared resolution deadline cannot be extended") {
            _ = try deadlineCache.resolve(host: host, route: route, timeout: 1, cancelled: { false }, current: { true },
                associated: { _, _, _, _ in deadlineClock.set(11); throw CampusPhysicalDNSError.timeout },
                encrypted: { _, _, _ in fatalError("no budget for DoH") })
        }
        expect(deadlineCache.count == 0, "expired resolution publishes no candidate")
        for completesBeforeDeadline in [true, false] {
            let coldClock = Clock(), coldCache = CampusPhysicalAddressResolver(now: { coldClock.now })
            let gate = ResolutionGate()
            let pending = Task.detached { () -> [String] in
                try coldCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in
                        coldClock.set(10.5); throw CampusPhysicalDNSError.unavailable
                    }, encrypted: { _, _, budget in
                        try gate.block(budget: budget)
                        return .init(addresses: values, ttl: 60)
                    })
            }
            expect(await gate.waitUntilEntered(), "cold encrypted request blocks after associated DNS failed")
            expect(gate.budget == 1.5 && coldCache.count == 0,
                   "cold encrypted lookup inherits the remaining original deadline and starts without a lease")
            coldClock.set(completesBeforeDeadline ? 11.99 : 12.01)
            gate.release()
            if completesBeforeDeadline {
                expect(try await pending.value == values, "late cold answer is accepted before the original deadline")
                expect(coldCache.count == 1, "timely cold answer alone creates a lease")
                let fresh = try coldCache.resolve(host: host, route: route, timeout: 2,
                    cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                    encrypted: { _, _, _ in fatalError("timely cold answer must remain fresh") })
                expect(fresh == values, "the next request can use the timely cold answer after associated DNS fails")
            } else {
                do { _ = try await pending.value; fatalError("late cold answer crossed its original deadline") }
                catch let error as CampusPhysicalTransportError {
                    expect(error == .timeout, "cold encrypted answer after the original deadline is rejected")
                }
                expect(coldCache.count == 0, "late cold success never populates a cache entry")
                fails(.dns, "the next request cannot recover from a secretly cached late answer") {
                    _ = try coldCache.resolve(host: host, route: route, timeout: 2,
                        cancelled: { false }, current: { true },
                        associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                        encrypted: { _, _, _ in throw CampusPhysicalTransportError.dns })
                }
                expect(coldCache.count == 0, "a subsequent failure still leaves the cold cache empty")
            }
        }
        let commitClock = CommitDeadlineClock(), commitCache = CampusPhysicalAddressResolver(now: { commitClock.now })
        fails(.timeout, "deadline crossing between reply acceptance and cache publication remains a timeout") {
            _ = try commitCache.resolve(host: host, route: route, timeout: 2,
                cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                encrypted: { _, _, _ in
                    commitClock.expireAfterReplyAcceptance()
                    return .init(addresses: values, ttl: 60)
                })
        }
        expect(commitCache.count == 0, "cache publication rechecks the original deadline before mutating entries")
        fails(.dns, "an expired cache commit cannot provide a candidate to the next request") {
            _ = try commitCache.resolve(host: host, route: route, timeout: 2,
                cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                encrypted: { _, _, _ in throw CampusPhysicalTransportError.dns })
        }
        cache.clear(); state.current = true
        fails(.cancelled, "late success from changed binding is not retained") {
            _ = try cache.resolve(host: host, route: route, timeout: 1, cancelled: { false }, current: { state.current },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable }, encrypted: { _, _, _ in
                    state.current = false; return .init(addresses: values, ttl: 60)
                })
        }
        expect(cache.count == 0, "late answer not cached")
        state.current = true
        let diagnosticTransport = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in true },
            resolve: { _, _, _, _ in throw CampusPhysicalDNSError.timeout }, addressResolver: CampusPhysicalAddressResolver(),
            encrypted: { _, _, _, _, _ in throw CampusPhysicalTransportError.timeout },
            transfer: { _, _, _, _, _, _, _, _ in fatalError("failed DNS cannot transfer") })
        do { _ = try await diagnosticTransport.dataWithDiagnostic(for: URLRequest(url: URL(string: "https://net.szu.edu.cn/")!), timeout: 2, limit: 100); fatalError("diagnostic failure expected") }
        catch let error as CampusPhysicalDiagnosticError {
            expect(error.underlying == .timeout && error.diagnostic == .init(stage: .resolution, reason: .resolutionTimeout), "per-request fixed diagnostics")
            let encoded = String(decoding: try JSONEncoder().encode(error.diagnostic), as: UTF8.self)
            expect(!encoded.contains("192.") && !encoded.contains("en99") && !encoded.contains("fake"), "diagnostic has no private route fields")
        }
        let raceClock = Clock(), raceCache = CampusPhysicalAddressResolver(now: { raceClock.now })
        _ = try raceCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
            associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
            encrypted: { _, _, _ in .init(addresses: values, ttl: 1) })
        raceClock.set(12)
        let entered = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
        let late = Task.detached { () -> [String] in
            try raceCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable }, encrypted: { _, _, _ in
                    entered.signal(); _ = release.wait(timeout: .now() + 5)
                    throw CampusPhysicalTransportError.dns
                })
        }
        let didEnter: Bool = await withCheckedContinuation { continuation in
            DispatchQueue.global().async { continuation.resume(returning: entered.wait(timeout: .now() + 5) == .success) }
        }
        expect(didEnter, "old request captured candidate before interleaving")
        let zero = try raceCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
            associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
            encrypted: { _, _, _ in .init(addresses: ["192.0.2.81"], ttl: 0) })
        expect(zero == ["192.0.2.81"] && raceCache.count == 0, "new valid zero TTL revokes old lease")
        release.signal()
        do { _ = try await late.value; fatalError("late DNS failure resurrected revoked candidate") }
        catch let error as CampusPhysicalTransportError { expect(error == .dns, "late old failure cannot reuse removed snapshot") }
        expect(raceCache.count == 0, "late request cannot recreate erased entry")
        for replacement in [false, true] {
            let epochClock = Clock(), epochCache = CampusPhysicalAddressResolver(now: { epochClock.now })
            _ = try epochCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                encrypted: { _, _, _ in .init(addresses: values, ttl: 1) })
            epochClock.set(12)
            let captured = DispatchSemaphore(value: 0), finish = DispatchSemaphore(value: 0)
            let old = Task.detached { () -> [String] in
                try epochCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable }, encrypted: { _, _, _ in
                        captured.signal(); _ = finish.wait(timeout: .now() + 5)
                        throw CampusPhysicalTransportError.dns
                    })
            }
            let didCapture: Bool = await withCheckedContinuation { continuation in
                DispatchQueue.global().async { continuation.resume(returning: captured.wait(timeout: .now() + 5) == .success) }
            }
            expect(didCapture, "old lease captured before explicit revocation or replacement")
            if replacement {
                _ = try epochCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                    encrypted: { _, _, _ in .init(addresses: ["192.0.2.82"], ttl: 60) })
            } else { epochCache.clear() }
            finish.signal()
            do { _ = try await old.value; fatalError("old request resurrected an obsolete address lease") }
            catch let error as CampusPhysicalTransportError { expect(error == .dns, "clear and replacement invalidate in-flight cached snapshot") }
            expect(epochCache.count == (replacement ? 1 : 0), "old failure cannot rebuild a cleared or replaced cache entry")
            if replacement {
                let retained = try epochCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                    encrypted: { _, _, _ in fatalError("replacement remains fresh") })
                expect(retained == ["192.0.2.82"], "new independent request sees only the replacement address")
            }
        }
        for update in ["same-address replacement", "Age-exhausted revocation", "unrelated-host replacement"] {
            let interleavedClock = Clock(), interleavedCache = CampusPhysicalAddressResolver(now: { interleavedClock.now })
            _ = try interleavedCache.resolve(host: host, route: route, timeout: 2,
                cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                encrypted: { _, _, _ in .init(addresses: values, ttl: 1) })
            interleavedClock.set(12)
            let gate = ResolutionGate()
            let old = Task.detached { () -> [String] in
                try interleavedCache.resolve(host: host, route: route, timeout: 2,
                    cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                    encrypted: { _, _, budget in
                        try gate.block(budget: budget); throw CampusPhysicalTransportError.dns
                    })
            }
            expect(await gate.waitUntilEntered(), "old HTTPS lease captured before \(update)")
            let otherHost = update == "unrelated-host replacement" ? "www.apple.com" : host
            let replacementAnswer = update == "Age-exhausted revocation"
                ? try CampusPhysicalAddressResolver.httpsAnswer(raw(packet(ttl: 600), headers: "Age: 600\r\n"),
                    host: host, transaction: 0x1234)
                : CampusPhysicalAddressResolver.Answer(addresses: values, ttl: 60)
            expect(try interleavedCache.resolve(host: otherHost, route: route, timeout: 2,
                cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                encrypted: { _, _, _ in replacementAnswer }) == values,
                   "interleaved \(update) returns its independently validated address")
            gate.release()
            if update == "unrelated-host replacement" {
                expect(try await old.value == values, "another hostname cannot revoke the original HTTPS lease")
                expect(interleavedCache.count == 2, "independent hostnames retain separate leases on the same physical route")
                let other = try interleavedCache.resolve(host: otherHost, route: route, timeout: 2,
                    cancelled: { false }, current: { true },
                    associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                    encrypted: { _, _, _ in fatalError("other-host replacement must remain fresh") })
                expect(other == values, "using the original lease leaves the other hostname's new lease intact")
            } else {
                do { _ = try await old.value; fatalError("old request reused a revoked or replaced lease") }
                catch let error as CampusPhysicalTransportError {
                    expect(error == .dns, "\(update) invalidates the old entry even when address bytes match")
                }
                expect(interleavedCache.count == (update == "Age-exhausted revocation" ? 0 : 1),
                       "old failure cannot recreate the lease after \(update)")
                if update == "same-address replacement" {
                    let fresh = try interleavedCache.resolve(host: host, route: route, timeout: 2,
                        cancelled: { false }, current: { true },
                        associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
                        encrypted: { _, _, _ in fatalError("same-address replacement must remain fresh") })
                    expect(fresh == values, "a new request can use the same-address replacement entry")
                }
            }
        }
        let ageClock = Clock(), ageCache = CampusPhysicalAddressResolver(now: { ageClock.now })
        _ = try ageCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
            associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable },
            encrypted: { _, _, _ in .init(addresses: values, ttl: 1) })
        ageClock.set(86_409.5)
        fails(.dns, "24h retention is checked again after the failed encrypted lookup") {
            _ = try ageCache.resolve(host: host, route: route, timeout: 2, cancelled: { false }, current: { true },
                associated: { _, _, _, _ in throw CampusPhysicalDNSError.unavailable }, encrypted: { _, _, _ in
                    ageClock.set(86_410.5); throw CampusPhysicalTransportError.dns
                })
        }
        expect(ageCache.count == 0, "a snapshot expiring during lookup is removed without extending retention")
        let budgets = State()
        let bounded = CampusPhysicalTransport(route: route, cookies: true, routeIsCurrent: { _ in true },
            resolve: { _, _, budget, _ in
                expect(budget <= 0.75 && budget > 0, "total budget reserves target connection")
                budgets.associated += 1; throw CampusPhysicalDNSError.unavailable
            }, addressResolver: CampusPhysicalAddressResolver(), encrypted: { name, actual, budget, _, current in
                expect(name == host && actual == route && current(), "DoH keeps exact hostname and physical binding")
                expect(budget <= 0.75 && budget > 0, "DoH does not reset total resolver budget")
                budgets.encrypted += 1; return .init(addresses: values, ttl: 60)
            }, transfer: { url, actual, addresses, cookie, budget, _, _, current in
                expect(url.host == host && actual == route && addresses == values && cookie.isEmpty && current(), "resolved address is only a bound connection candidate")
                expect(budget > 0.15 && budget <= 1, "target receives remaining shared deadline")
                budgets.transfer += 1
                return .init(data: Data("fake".utf8), headers: "HTTP/1.1 200 OK\r\n\r\n", status: 200)
            })
        _ = try await bounded.dataWithDiagnostic(for: URLRequest(url: URL(string: "https://net.szu.edu.cn/")!), timeout: 1, limit: 100)
        expect(budgets.associated == 1 && budgets.encrypted == 1 && budgets.transfer == 1, "one bounded resolver chain and one target access")
        let changing = State()
        let changed = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in changing.current },
            resolve: { _, _, _, _ in changing.current = false; return values }, addressResolver: CampusPhysicalAddressResolver(),
            encrypted: { _, _, _, _, _ in fatalError("binding failure cannot start DoH") },
            transfer: { _, _, _, _, _, _, _, _ in fatalError("binding failure cannot transfer") })
        do { _ = try await changed.dataWithDiagnostic(for: URLRequest(url: URL(string: "https://net.szu.edu.cn/")!), timeout: 1, limit: 100); fatalError("binding failure expected") }
        catch let error as CampusPhysicalDiagnosticError {
            expect(error.diagnostic == .init(stage: .binding, reason: .bindingChanged), "binding failure distinct from DNS or campus identity")
        }
        let dnsTLSState = State()
        let dnsTLS = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in true },
            resolve: { _, _, _, _ in throw CampusPhysicalTransportError.dns },
            addressResolver: CampusPhysicalAddressResolver(),
            encrypted: { _, _, _, _, _ in dnsTLSState.encrypted += 1; throw CampusPhysicalTransportError.tls },
            transfer: { _, _, _, _, _, _, _, _ in dnsTLSState.transfer += 1; throw CampusPhysicalTransportError.network })
        do {
            _ = try await dnsTLS.dataWithDiagnostic(for: URLRequest(url: URL(string: "https://net.szu.edu.cn/")!), timeout: 1, limit: 100)
            fatalError("resolver TLS failure expected")
        } catch let error as CampusPhysicalDiagnosticError {
            expect(error.underlying == .tls && error.diagnostic == .init(stage: .resolution, reason: .dnsTlsFailed),
                   "resolver HTTPS failure retains its resolution phase and terminal TLS classification")
        }
        expect(dnsTLSState.encrypted == 1 && dnsTLSState.transfer == 0,
               "resolver TLS terminates before target transfer without a second resolver or authentication")
        for (error, diagnostic) in [(CampusPhysicalTransportError.tls, CampusPhysicalDiagnostic(stage: .tls, reason: .tlsFailed)),
                                     (.network, .init(stage: .connection, reason: .connectionFailed)),
                                     (.timeout, .init(stage: .connection, reason: .connectionTimeout))] {
            let failed = CampusPhysicalTransport(route: route, cookies: false, routeIsCurrent: { _ in true },
                resolve: { _, _, _, _ in values }, addressResolver: CampusPhysicalAddressResolver(),
                transfer: { _, _, _, _, _, _, _, _ in throw error })
            do { _ = try await failed.dataWithDiagnostic(for: URLRequest(url: URL(string: "https://net.szu.edu.cn/")!), timeout: 1, limit: 100); fatalError("fixed failure expected") }
            catch let failed as CampusPhysicalDiagnosticError { expect(failed.diagnostic == diagnostic, "target failure keeps its own fixed stage") }
        }
        print("BoundedResolution: \(assertions) assertions passed (fake packets, clocks and services; zero external requests).")
    }
}
#endif
