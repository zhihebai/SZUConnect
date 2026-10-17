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

actor CampusPhysicalTransport {
    typealias Resolver = @Sendable (String, CampusPhysicalRoute, TimeInterval, @Sendable () -> Bool) throws -> [String]
    typealias Transfer = @Sendable (URL, CampusPhysicalRoute, [String], String, TimeInterval, Int, CampusPhysicalCancellation, @escaping @Sendable () -> Bool) throws -> CampusPhysicalTransferResult
    private let route: CampusPhysicalRoute
    private let cookiesEnabled: Bool
    private let routeIsCurrent: @Sendable (CampusPhysicalRoute) -> Bool
    private let resolve: Resolver
    private let transfer: Transfer
    private var generation: UInt64 = 0
    private var pending: [UUID: CampusPhysicalCancellation] = [:]
    private var cookieJar = CampusPhysicalCookieJar()

    init(route: CampusPhysicalRoute, cookies: Bool,
         routeIsCurrent: @escaping @Sendable (CampusPhysicalRoute) -> Bool = { CampusPhysicalNetwork.capture(interfaceName: $0.interfaceName) == .bound($0) },
         resolve: @escaping Resolver = { try CampusPhysicalDNS.resolveA(host: $0, route: $1, timeout: $2, cancelled: $3) },
         transfer: @escaping Transfer = { try CampusPhysicalTransport.perform(url: $0, route: $1, addresses: $2, cookie: $3, timeout: $4, limit: $5, cancellation: $6, current: $7) }) {
        self.route = route
        cookiesEnabled = cookies
        self.routeIsCurrent = routeIsCurrent
        self.resolve = resolve
        self.transfer = transfer
    }

    func cancel() async {
        generation &+= 1
        for cancellation in pending.values { cancellation.cancel() }
        pending.removeAll()
        cookieJar.removeAll()
    }

    deinit { for cancellation in pending.values { cancellation.cancel() } }

    func data(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
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
        let raw = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<CampusPhysicalTransferResult, Error>) in
                DispatchQueue.global(qos: .userInitiated).async {
                    do {
                        let start = ProcessInfo.processInfo.systemUptime
                        guard !cancellation.isCancelled, routeIsCurrent(route) else { throw CampusPhysicalTransportError.cancelled }
                        let host = url.host!
                        let addresses: [String]
                        if host == "172.30.255.42" { addresses = [] }
                        else {
                            do {
                                addresses = try resolve(host, route, timeout, { cancellation.isCancelled })
                            } catch let error as CampusPhysicalDNSError {
                                switch error {
                                case .cancelled: throw CampusPhysicalTransportError.cancelled
                                case .timeout: throw CampusPhysicalTransportError.timeout
                                case .unavailable, .invalidResponse: throw CampusPhysicalTransportError.dns
                                }
                            }
                        }
                        guard !cancellation.isCancelled, routeIsCurrent(route) else { throw CampusPhysicalTransportError.cancelled }
                        let remaining = timeout - (ProcessInfo.processInfo.systemUptime - start)
                        guard remaining > 0 else { throw CampusPhysicalTransportError.timeout }
                        let result = try transfer(url, route, addresses, cookie, remaining, limit, cancellation, { routeIsCurrent(route) })
                        guard !cancellation.isCancelled else { throw CampusPhysicalTransportError.cancelled }
                        continuation.resume(returning: result)
                    } catch { continuation.resume(throwing: error) }
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
                                           timeout: TimeInterval, limit: Int, cancellation: CampusPhysicalCancellation, current: @escaping @Sendable () -> Bool) throws -> CampusPhysicalTransferResult {
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
                            var response = szu_campus_request(&request, cancellation.pointer)
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
