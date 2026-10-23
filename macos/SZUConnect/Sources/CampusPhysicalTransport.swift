#if os(macOS)
import Foundation
import Darwin

enum CampusPhysicalTransportError: Error, Equatable {
    case cancelled, timeout, dns, tls, network, response, responseTooLarge
}

/// Each request owns its cancellation token until DNS and libcurl have both returned.
/// Cancelling sets the flag and wakes the transfer; the worker retains and releases its token.
final class CampusPhysicalCancellation: @unchecked Sendable {
    let pointer: OpaquePointer
    init() throws {
        guard let pointer = szu_campus_cancellation_create() else { throw CampusPhysicalTransportError.network }
        self.pointer = pointer
    }
    func cancel() { szu_campus_cancellation_cancel(pointer) }
    var isCancelled: Bool { szu_campus_cancellation_is_cancelled(pointer) != 0 }
    deinit { szu_campus_cancellation_release(pointer) }
}

private final class CampusPhysicalEpoch: @unchecked Sendable {
    let current: @Sendable () -> Bool
    init(_ current: @escaping @Sendable () -> Bool) { self.current = current }
}

struct CampusPhysicalTransferResult: Sendable {
    let data: Data
    let headers: String
    let status: Int
}

/// RAM-only cookies are intentionally separate from URLSession/HTTPCookieStorage.
struct CampusPhysicalCookieJar {
    private struct Entry {
        let cookie: HTTPCookie
        let host: String
        let expires: Date?
    }
    private var entries: [Entry] = []

    mutating func removeAll() { entries.removeAll() }

    mutating func update(lines: [String], for url: URL, now: Date = Date()) {
        guard let host = url.host?.lowercased() else { return }
        entries.removeAll { $0.expires.map { $0 <= now } ?? false }
        for line in lines.prefix(64) where line.utf8.count <= 8192 && !line.contains("\r") && !line.contains("\n") {
            for cookie in HTTPCookie.cookies(withResponseHeaderFields: ["Set-Cookie": line], for: url) {
                let domain = cookie.domain.hasPrefix(".") ? String(cookie.domain.dropFirst()) : cookie.domain
                guard domain.lowercased() == host, !cookie.name.isEmpty,
                      !cookie.name.contains(";"), !cookie.value.contains(";"),
                      !(cookie.name + cookie.value).contains("\r"), !(cookie.name + cookie.value).contains("\n"),
                      cookie.path.hasPrefix("/"), !cookie.isSecure || url.scheme == "https" else { continue }
                let maximumAge = line.split(separator: ";").dropFirst().compactMap { attribute -> TimeInterval? in
                    let parts = attribute.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
                    guard parts.count == 2, parts[0].lowercased() == "max-age", let seconds = Int64(parts[1]) else { return nil }
                    return TimeInterval(seconds)
                }.first
                let expires = maximumAge.map { now.addingTimeInterval($0) } ?? cookie.expiresDate
                entries.removeAll { $0.host == host && $0.cookie.name == cookie.name && $0.cookie.path == cookie.path }
                if expires.map({ $0 <= now }) != true { entries.append(Entry(cookie: cookie, host: host, expires: expires)) }
            }
        }
        // Bound retained state even if the server returns a different cookie on every request.
        while entries.count > 64 || entries.reduce(0, { $0 + $1.cookie.name.utf8.count + $1.cookie.value.utf8.count }) > 8192 {
            entries.removeFirst()
        }
    }

    func header(for url: URL, now: Date = Date()) -> String {
        guard let host = url.host?.lowercased() else { return "" }
        return entries.filter { entry in
            entry.host == host && (entry.expires.map { $0 > now } ?? true) &&
            (!entry.cookie.isSecure || url.scheme == "https") && Self.pathMatches(url.path, cookiePath: entry.cookie.path)
        }.sorted { $0.cookie.path.count > $1.cookie.path.count }
            .map { "\($0.cookie.name)=\($0.cookie.value)" }.joined(separator: "; ")
    }

    private static func pathMatches(_ path: String, cookiePath: String) -> Bool {
        guard path.hasPrefix(cookiePath) else { return false }
        return path == cookiePath || cookiePath.hasSuffix("/") || path.dropFirst(cookiePath.count).first == "/"
    }
}

/// Only fixed enums leave this layer. These fields contain no network or credential text.
struct CampusPhysicalDiagnostic: Codable, Equatable, Sendable {
    enum Stage: String, Codable, Sendable { case resolution, tls, connection, http, page, binding }
    enum Reason: String, Codable, Sendable {
        case resolutionTimeout, resolutionFailed, dnsTlsFailed, tlsFailed, connectionTimeout, connectionFailed
        case httpRejected, malformedResponse, responseTooLarge, bindingChanged, cancelled
    }
    let stage: Stage
    let reason: Reason
}
struct CampusPhysicalDiagnosticError: Error, Sendable {
    let underlying: CampusPhysicalTransportError
    let diagnostic: CampusPhysicalDiagnostic
}

/// Connection candidates only: this cache cannot supply campus identity, trust or a proof.
/// Process-wide RAM survives short-lived request actors, but never process exit.
final class CampusPhysicalAddressResolver: @unchecked Sendable {
    struct Answer: Sendable { let addresses: [String]; let ttl: TimeInterval }
    typealias Associated = @Sendable (String, CampusPhysicalRoute, TimeInterval, @Sendable () -> Bool) throws -> [String]
    typealias Encrypted = @Sendable (String, CampusPhysicalRoute, TimeInterval) throws -> Answer
    private struct Entry {
        let id: UUID
        let host: String, route: CampusPhysicalRoute, addresses: [String]
        let freshUntil: TimeInterval, retainUntil: TimeInterval
    }
    static let shared = CampusPhysicalAddressResolver()
    private let lock = NSLock()
    private var entries: [Entry] = []
    private let now: @Sendable () -> TimeInterval
    init(now: @escaping @Sendable () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) { self.now = now }

    func clear() { lock.lock(); defer { lock.unlock() }; entries.removeAll() }
    var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    private func cached(_ host: String, route: CampusPhysicalRoute, now: TimeInterval) -> Entry? {
        lock.lock(); defer { lock.unlock() }
        entries.removeAll { now >= $0.retainUntil }
        return entries.first { $0.host == host && $0.route == route }
    }
    private func retained(_ entry: Entry, stale: Bool) -> [String]? {
        let instant = now()
        lock.lock(); defer { lock.unlock() }
        entries.removeAll { instant >= $0.retainUntil }
        // Another in-flight answer (including TTL zero) may revoke this snapshot.
        // Never resurrect it merely because the older resolver failed later.
        guard let current = entries.first(where: { $0.id == entry.id }),
              instant < (stale ? current.retainUntil : current.freshUntil) else { return nil }
        return current.addresses
    }
    private func remember(_ answer: Answer, host: String, route: CampusPhysicalRoute,
                          began: TimeInterval, deadline: TimeInterval) throws {
        let ttl = answer.ttl.isFinite ? min(300, max(0, answer.ttl)) : 0
        lock.lock(); defer { lock.unlock() }
        // Publication must still belong to this request's original budget. The
        // deadline may pass after accepting the reply or while waiting for this lock.
        let now = self.now()
        guard now.isFinite, now >= began else { throw CampusPhysicalTransportError.dns }
        guard now < deadline else { throw CampusPhysicalTransportError.timeout }
        // Even a valid zero/invalid/Age-exhausted TTL revokes the preceding entry.
        entries.removeAll { now >= $0.retainUntil || ($0.host == host && $0.route == route) }
        guard ttl > 0 else { return }
        if entries.count >= 24 { entries.removeFirst() }
        let retention: TimeInterval = Self.httpsHost(host) ? 86_400 : ttl
        entries.append(.init(id: UUID(), host: host, route: route, addresses: answer.addresses,
                             freshUntil: now + ttl, retainUntil: now + retention))
    }
    private static func httpsHost(_ host: String) -> Bool { host == "net.szu.edu.cn" || host == "www.apple.com" }
    private static func valid(_ addresses: [String]) -> Bool {
        !addresses.isEmpty && addresses.count <= 16 && addresses.allSatisfy(CampusPhysicalDNS.isUsableIPv4)
    }
    func resolve(host: String, route: CampusPhysicalRoute, timeout: TimeInterval,
                 cancelled: @escaping @Sendable () -> Bool, current: @escaping @Sendable () -> Bool,
                 associated: Associated, encrypted: Encrypted) throws -> [String] {
        guard CampusPhysicalDNS.allowedHosts.contains(host), timeout.isFinite, timeout > 0, timeout <= 12,
              !route.identity.isEmpty, !route.interfaceName.isEmpty, route.interfaceIndex > 0,
              Self.valid(route.addresses), !route.dnsServers.isEmpty, route.dnsServers.count <= 8,
              route.dnsServers.allSatisfy(CampusPhysicalDNS.isUsableIPv4) else { throw CampusPhysicalTransportError.dns }
        let began = now(), deadline = began + timeout
        guard began.isFinite else { throw CampusPhysicalTransportError.dns }
        func remaining() throws -> TimeInterval {
            let value = now()
            guard value.isFinite, value >= began else { throw CampusPhysicalTransportError.dns }
            return max(0, deadline - value)
        }
        func check() throws {
            guard !cancelled(), current(), !cancelled() else { throw CampusPhysicalTransportError.cancelled }
        }
        func accept(_ addresses: [String]) throws -> [String] {
            try check()
            guard try remaining() > 0 else { throw CampusPhysicalTransportError.timeout }
            guard Self.valid(addresses) else { throw CampusPhysicalTransportError.dns }
            return addresses
        }
        try check()
        var failure: CampusPhysicalTransportError = .dns
        do {
            let budget = min(1, try remaining())
            guard budget > 0 else { throw CampusPhysicalTransportError.timeout }
            return try accept(associated(host, route, budget, { cancelled() || !current() }))
        } catch let error as CampusPhysicalDNSError {
            if error == .cancelled { throw CampusPhysicalTransportError.cancelled }
            failure = error == .timeout ? .timeout : .dns
        } catch let error as CampusPhysicalTransportError {
            if error == .cancelled || error == .tls { throw error }; failure = error == .timeout ? .timeout : .dns
        }
        try check()
        let previous = cached(host, route: route, now: now())
        guard try remaining() > 0 else { throw CampusPhysicalTransportError.timeout }
        if let previous, let addresses = retained(previous, stale: false) { return try accept(addresses) }
        do {
            let reserve: TimeInterval = previous != nil && Self.httpsHost(host) ? 0.1 : 0
            let budget = try remaining() - reserve
            guard budget > 0 else { throw CampusPhysicalTransportError.timeout }
            let answer = try encrypted(host, route, budget)
            _ = try accept(answer.addresses)
            try check()
            try remember(answer, host: host, route: route, began: began, deadline: deadline)
            return try accept(answer.addresses)
        } catch let error as CampusPhysicalTransportError {
            if error == .cancelled || error == .tls { throw error }; failure = error == .timeout ? .timeout : .dns
        } catch let error as CampusPhysicalDNSError {
            if error == .cancelled { throw CampusPhysicalTransportError.cancelled }
            failure = error == .timeout ? .timeout : .dns
        }
        try check()
        guard try remaining() > 0 else { throw CampusPhysicalTransportError.timeout }
        if let previous, Self.httpsHost(host), let addresses = retained(previous, stale: true) { return try accept(addresses) }
        throw failure
    }

    static func httpsAnswer(_ raw: CampusPhysicalTransferResult, host: String, transaction: UInt16) throws -> Answer {
        guard raw.status == 200, raw.data.count <= 65_535, raw.headers.utf8.count <= 65_535, raw.headers.hasSuffix("\r\n\r\n"),
              raw.headers.components(separatedBy: "\r\n\r\n").count == 2 else {
            throw CampusPhysicalTransportError.dns
        }
        let parsed: (fields: [String: String], cookies: [String])
        do { parsed = try CampusPhysicalTransport.parseHeaders(raw.headers, status: raw.status, limit: 65_535) }
        catch { throw CampusPhysicalTransportError.dns }
        let headers = Dictionary(uniqueKeysWithValues: parsed.fields.map { ($0.key.lowercased(), $0.value) })
        guard headers["content-type"]?.lowercased() == "application/dns-message",
              headers["content-encoding"].map({ $0.lowercased() == "identity" }) ?? true else {
            throw CampusPhysicalTransportError.dns
        }
        let addresses = try CampusPhysicalDNS.parse(raw.data, host: host, transaction: transaction)
        var ttl = try CampusPhysicalDNS.responseTTL(raw.data, host: host, transaction: transaction)
        if let value = headers["age"] {
            if value.isEmpty || value.utf8.count > 20 || !value.utf8.allSatisfy({ (48...57).contains($0) }) { ttl = 0 }
            else if let age = UInt64(value) { ttl = max(0, ttl - TimeInterval(age)) }
            else { ttl = 0 }
        }
        return Answer(addresses: addresses, ttl: min(300, ttl))
    }
}

actor CampusPhysicalTransport {
    typealias Resolver = @Sendable (String, CampusPhysicalRoute, TimeInterval, @Sendable () -> Bool) throws -> [String]
    typealias Transfer = @Sendable (URL, CampusPhysicalRoute, [String], String, TimeInterval, Int, CampusPhysicalCancellation, @escaping @Sendable () -> Bool) throws -> CampusPhysicalTransferResult
    private let route: CampusPhysicalRoute
    private let cookiesEnabled: Bool
    private let routeIsCurrent: @Sendable (CampusPhysicalRoute) -> Bool
    private let resolve: Resolver
    private let transfer: Transfer
    private let addressResolver: CampusPhysicalAddressResolver
    private let encrypted: (@Sendable (String, CampusPhysicalRoute, TimeInterval, CampusPhysicalCancellation, @escaping @Sendable () -> Bool) throws -> CampusPhysicalAddressResolver.Answer)?
    private var generation: UInt64 = 0
    private var pending: [UUID: CampusPhysicalCancellation] = [:]
    private var cookieJar = CampusPhysicalCookieJar()

    init(route: CampusPhysicalRoute, cookies: Bool,
         routeIsCurrent: @escaping @Sendable (CampusPhysicalRoute) -> Bool = { CampusPhysicalNetwork.capture(interfaceName: $0.interfaceName) == .bound($0) },
         resolve: @escaping Resolver = { try CampusPhysicalDNS.resolveA(host: $0, route: $1, timeout: $2, cancelled: $3) },
         addressResolver: CampusPhysicalAddressResolver = .shared,
         encrypted: (@Sendable (String, CampusPhysicalRoute, TimeInterval, CampusPhysicalCancellation, @escaping @Sendable () -> Bool) throws -> CampusPhysicalAddressResolver.Answer)? = nil,
         transfer: @escaping Transfer = { try CampusPhysicalTransport.perform(url: $0, route: $1, addresses: $2, cookie: $3, timeout: $4, limit: $5, cancellation: $6, current: $7) }) {
        self.route = route
        cookiesEnabled = cookies
        self.routeIsCurrent = routeIsCurrent
        self.resolve = resolve
        self.transfer = transfer
        self.addressResolver = addressResolver
        self.encrypted = encrypted
    }

    func cancel() async {
        generation &+= 1
        for cancellation in pending.values { cancellation.cancel() }
        pending.removeAll()
        cookieJar.removeAll()
    }

    deinit { for cancellation in pending.values { cancellation.cancel() } }

    func data(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
        do { return try await dataWithDiagnostic(for: request, timeout: timeout, limit: limit) }
        catch let error as CampusPhysicalDiagnosticError { throw error.underlying }
    }

    func dataWithDiagnostic(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
        do { return try await execute(for: request, timeout: timeout, limit: limit) }
        catch let error as CampusPhysicalDiagnosticError { throw error }
        catch {
            let underlying = (error as? CampusPhysicalTransportError) ?? .response
            throw CampusPhysicalDiagnosticError(underlying: underlying,
                diagnostic: Self.diagnostic(underlying, stage: .page, current: routeIsCurrent(route), cancelled: Task.isCancelled))
        }
    }

    private func execute(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
        guard !Task.isCancelled else { throw CampusPhysicalTransportError.cancelled }
        guard let url = request.url, Self.isAllowed(url), request.httpMethod == nil || request.httpMethod == "GET",
              request.httpBody == nil, request.httpBodyStream == nil,
              (request.allHTTPHeaderFields ?? [:]).keys.allSatisfy({ $0.lowercased() == "cache-control" }),
              timeout.isFinite, timeout > 0, timeout <= 12, limit > 0, limit <= 1_048_576,
              !route.interfaceName.isEmpty, route.interfaceIndex > 0 else { throw CampusPhysicalTransportError.response }
        guard routeIsCurrent(route) else { throw CampusPhysicalTransportError.cancelled }
        let requestID = UUID()
        let requestGeneration = generation
        let cancellation = try CampusPhysicalCancellation()
        pending[requestID] = cancellation
        defer { pending.removeValue(forKey: requestID) }
        let cookie = cookiesEnabled ? cookieJar.header(for: url) : ""
        let route = self.route, resolve = self.resolve, transfer = self.transfer, routeIsCurrent = self.routeIsCurrent
        let addressResolver = self.addressResolver, encrypted = self.encrypted
        let raw = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<CampusPhysicalTransferResult, Error>) in
                DispatchQueue.global(qos: .userInitiated).async {
                    var stage: CampusPhysicalDiagnostic.Stage = .binding
                    do {
                        let start = ProcessInfo.processInfo.systemUptime
                        guard !cancellation.isCancelled, routeIsCurrent(route) else { throw CampusPhysicalTransportError.cancelled }
                        let host = url.host!
                        let addresses: [String]
                        if host == "172.30.255.42" { addresses = [] }
                        else {
                            stage = .resolution
                            // Resolver and target share one total deadline. Leave a connection
                            // budget rather than spending the entire request on two resolvers.
                            let reserve = min(1, max(0.15, timeout * 0.25))
                            let budget = timeout - reserve
                            guard budget > 0 else { throw CampusPhysicalTransportError.timeout }
                            addresses = try addressResolver.resolve(host: host, route: route, timeout: budget,
                                cancelled: { cancellation.isCancelled }, current: { routeIsCurrent(route) },
                                associated: resolve, encrypted: { host, route, budget in
                                    let current: @Sendable () -> Bool = { routeIsCurrent(route) }
                                    if let encrypted { return try encrypted(host, route, budget, cancellation, current) }
                                    let transaction = UInt16.random(in: .min ... .max)
                                    let raw = try Self.perform(url: URL(string: "https://dns.alidns.com/")!, route: route,
                                        addresses: [], cookie: "", timeout: budget, limit: 65_535,
                                        cancellation: cancellation, current: current, dnsHost: host, transaction: transaction)
                                    return try CampusPhysicalAddressResolver.httpsAnswer(raw, host: host, transaction: transaction)
                                })
                        }
                        stage = .binding
                        guard !cancellation.isCancelled, routeIsCurrent(route) else { throw CampusPhysicalTransportError.cancelled }
                        let remaining = timeout - (ProcessInfo.processInfo.systemUptime - start)
                        guard remaining > 0 else { throw CampusPhysicalTransportError.timeout }
                        stage = .connection
                        let result = try transfer(url, route, addresses, cookie, remaining, limit, cancellation, { routeIsCurrent(route) })
                        guard !cancellation.isCancelled else { throw CampusPhysicalTransportError.cancelled }
                        continuation.resume(returning: result)
                    } catch {
                        let underlying = (error as? CampusPhysicalTransportError) ?? .network
                        let diagnostic = Self.diagnostic(underlying, stage: stage, current: routeIsCurrent(route), cancelled: cancellation.isCancelled)
                        continuation.resume(throwing: CampusPhysicalDiagnosticError(underlying: underlying, diagnostic: diagnostic))
                    }
                }
            }
        } onCancel: { cancellation.cancel() }
        guard !Task.isCancelled, !cancellation.isCancelled, requestGeneration == generation, routeIsCurrent(route) else {
            throw CampusPhysicalTransportError.cancelled
        }
        guard raw.data.count <= limit else { throw CampusPhysicalTransportError.responseTooLarge }
        let parsed = try Self.parseHeaders(raw.headers, status: raw.status, limit: limit)
        guard let response = HTTPURLResponse(url: url, statusCode: raw.status, httpVersion: nil, headerFields: parsed.fields) else {
            throw CampusPhysicalTransportError.response
        }
        if cookiesEnabled { cookieJar.update(lines: parsed.cookies, for: url) }
        return (raw.data, response)
    }

    private nonisolated static func diagnostic(_ error: CampusPhysicalTransportError, stage: CampusPhysicalDiagnostic.Stage,
                                               current: Bool, cancelled: Bool) -> CampusPhysicalDiagnostic {
        if cancelled { return .init(stage: .binding, reason: .cancelled) }
        if !current { return .init(stage: .binding, reason: .bindingChanged) }
        switch error {
        case .cancelled: return .init(stage: .binding, reason: .cancelled)
        case .timeout: return .init(stage: stage, reason: stage == .resolution ? .resolutionTimeout : .connectionTimeout)
        case .dns: return .init(stage: .resolution, reason: .resolutionFailed)
        case .tls:
            // The resolver's HTTPS identity and the campus endpoint's HTTPS
            // identity are independent. Preserve the failed phase without
            // weakening the shared terminal TLS error used by authentication.
            return stage == .resolution ? .init(stage: .resolution, reason: .dnsTlsFailed)
                : .init(stage: .tls, reason: .tlsFailed)
        case .network: return .init(stage: stage, reason: .connectionFailed)
        case .response: return .init(stage: .page, reason: .malformedResponse)
        case .responseTooLarge: return .init(stage: .page, reason: .responseTooLarge)
        }
    }

    static func isAllowed(_ url: URL) -> Bool {
        guard url.user == nil, url.password == nil, url.fragment == nil, url.absoluteString.utf8.count <= 65_536 else { return false }
        switch (url.scheme, url.host, url.port) {
        case ("https", "net.szu.edu.cn", nil), ("https", "net.szu.edu.cn", 443):
            return (["/", "/srun_portal_pc"].contains(url.path) && url.query == nil) || ["/cgi-bin/get_challenge", "/cgi-bin/srun_portal"].contains(url.path)
        case ("http", "172.30.255.42", 801):
            return (["/", "/eportal"].contains(url.path) && url.query == nil) || url.path == "/eportal/portal/login"
        case ("https", "www.apple.com", nil), ("https", "www.apple.com", 443):
            return url.path == "/library/test/success.html" && url.query == nil
        case ("http", "www.msftconnecttest.com", nil), ("http", "www.msftconnecttest.com", 80):
            return url.path == "/connecttest.txt" && url.query == nil
        default: return false
        }
    }

    static func parseHeaders(_ text: String, status: Int, limit: Int) throws -> (fields: [String: String], cookies: [String]) {
        guard text.utf8.count <= 65_536 else { throw CampusPhysicalTransportError.responseTooLarge }
        let lines = text.components(separatedBy: "\r\n")
        guard let first = lines.first, first.hasPrefix("HTTP/"),
              first.split(separator: " ").dropFirst().first.flatMap({ Int($0) }) == status,
              (100...599).contains(status) else { throw CampusPhysicalTransportError.response }
        var fields: [String: String] = [:], names: [String: String] = [:], cookies: [String] = []
        let token = CharacterSet(charactersIn: "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
        for line in lines.dropFirst() {
            if line.isEmpty { break } // Trailers cannot add authentication cookies.
            guard !line.hasPrefix(" "), !line.hasPrefix("\t"), !line.contains("\n"), !line.contains("\0"),
                  let colon = line.firstIndex(of: ":") else { throw CampusPhysicalTransportError.response }
            let name = String(line[..<colon]), value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty, name.unicodeScalars.allSatisfy({ token.contains($0) }),
                  value.unicodeScalars.allSatisfy({ $0.value == 9 || ($0.value >= 32 && $0.value != 127) }) else { throw CampusPhysicalTransportError.response }
            let lower = name.lowercased()
            if lower == "set-cookie" { cookies.append(value); continue }
            if lower == "content-length" {
                guard let length = UInt64(value) else { throw CampusPhysicalTransportError.response }
                guard length <= UInt64(limit) else { throw CampusPhysicalTransportError.responseTooLarge }
            }
            if let original = names[lower] { fields[original, default: ""] += ", " + value }
            else { names[lower] = name; fields[name] = value }
        }
        return (fields, cookies)
    }

    private nonisolated static func perform(url: URL, route: CampusPhysicalRoute, addresses: [String], cookie: String,
                                           timeout: TimeInterval, limit: Int, cancellation: CampusPhysicalCancellation, current: @escaping @Sendable () -> Bool, dnsHost: String? = nil, transaction: UInt16 = 0) throws -> CampusPhysicalTransferResult {
        let resolved = addresses.joined(separator: ",")
        guard let source = route.addresses.first else { throw CampusPhysicalTransportError.network }
        let epoch = Unmanaged.passRetained(CampusPhysicalEpoch(current))
        defer { epoch.release() }
        return try source.withCString { sourcePointer in
            try route.interfaceName.withCString { interfacePointer in
                try url.absoluteString.withCString { urlPointer in
                    try resolved.withCString { addressPointer in
                        try cookie.withCString { cookiePointer in
                            var request = SZUCampusRequest(url: urlPointer, resolved_addresses: addressPointer, cookie: cookiePointer,
                                source_ipv4: sourcePointer, interface_name: interfacePointer, route_is_current: { context in
                                    guard let context else { return 0 }
                                    return Unmanaged<CampusPhysicalEpoch>.fromOpaque(context).takeUnretainedValue().current() ? 1 : 0
                                }, route_context: epoch.toOpaque(), interface_index: route.interfaceIndex, timeout_seconds: timeout, response_limit: limit)
                            var response: SZUCampusResponse
                            if let dnsHost { response = dnsHost.withCString { szu_campus_dns_request(&request, $0, transaction, cancellation.pointer) } }
                            else { response = szu_campus_request(&request, cancellation.pointer) }
                            defer { szu_campus_response_release(&response) }
                            guard response.error == SZU_CAMPUS_OK else {
                                switch Int(response.error) {
                                case SZU_CAMPUS_CANCELLED: throw CampusPhysicalTransportError.cancelled
                                case SZU_CAMPUS_TIMEOUT: throw CampusPhysicalTransportError.timeout
                                case SZU_CAMPUS_DNS: throw CampusPhysicalTransportError.dns
                                case SZU_CAMPUS_TLS: throw CampusPhysicalTransportError.tls
                                case SZU_CAMPUS_TOO_LARGE: throw CampusPhysicalTransportError.responseTooLarge
                                case SZU_CAMPUS_RESPONSE: throw CampusPhysicalTransportError.response
                                default: throw CampusPhysicalTransportError.network
                                }
                            }
                            guard let body = response.body, let headers = response.headers,
                                  let text = String(data: Data(bytes: headers, count: response.headers_length), encoding: .isoLatin1) else {
                                throw CampusPhysicalTransportError.response
                            }
                            return CampusPhysicalTransferResult(data: Data(bytes: body, count: response.body_length), headers: text, status: response.status)
                        }
                    }
                }
            }
        }
    }
}
#endif
