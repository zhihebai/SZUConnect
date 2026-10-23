import Foundation
import CoreFoundation

/// Authentication targets are deliberately fixed to the upstream campus endpoints.
enum CampusArea: String, CaseIterable, Identifiable, Codable, Sendable {
    case automatic, teaching, dormitory
    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: SharedCopy.t("area.automatic.title")
        case .teaching: SharedCopy.t("area.teaching.title")
        case .dormitory: SharedCopy.t("area.dormitory.title")
        }
    }
    var symbol: String {
        switch self {
        case .automatic: "sparkles"
        case .teaching: "building.2"
        case .dormitory: "house"
        }
    }
    /// One plain sentence shown under the area picker.
    var explanation: String {
        switch self {
        case .automatic: SharedCopy.t("area.automatic.detail")
        case .teaching: SharedCopy.t("area.teaching.detail")
        case .dormitory: SharedCopy.t("area.dormitory.detail")
        }
    }
}

struct Credentials: Codable, Sendable {
    var account: String
    var password: String
}

enum PortalError: Error, LocalizedError {
    case invalidCredentials, network, response, rejected, cryptography, forbiddenTarget, tls
    case timeout, dns, redirect, httpStatus, responseTooLarge, authenticationUncertain, protocolConflict
    var errorDescription: String? {
        switch self {
        case .invalidCredentials: SharedCopy.t("portalError.invalidCredentials")
        case .network: SharedCopy.t("portalError.network")
        case .timeout: SharedCopy.t("portalError.timeout")
        case .dns: SharedCopy.t("portalError.dns")
        case .redirect: SharedCopy.t("portalError.redirect")
        case .httpStatus: SharedCopy.t("portalError.httpStatus")
        case .responseTooLarge: SharedCopy.t("portalError.responseTooLarge")
        case .authenticationUncertain: SharedCopy.t("portalError.authenticationUncertain")
        case .protocolConflict: SharedCopy.t("portalError.protocolConflict")
        case .tls: SharedCopy.t("portalError.tls")
        case .response: SharedCopy.t("portalError.response")
        case .rejected: SharedCopy.t("portalError.rejected")
        case .cryptography: SharedCopy.t("portalError.cryptography")
        case .forbiddenTarget: SharedCopy.t("portalError.forbiddenTarget")
        }
    }
}

private final class NoRedirectDelegate: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

enum ConnectivityProbeMode: String, CaseIterable, Identifiable, Codable, Sendable {
    case quick, complete
    var id: String { rawValue }
    var title: String { self == .quick ? SharedCopy.t("probe.quick.title") : SharedCopy.t("probe.complete.title") }
    var detail: String {
        self == .quick ? SharedCopy.t("probe.quick.detail")
            : SharedCopy.t("probe.complete.detail")
    }
}

enum ConnectivityEndpointState: String, Sendable {
    case success, timeout, dns, tls, redirect, unexpectedResponse, network, cancelled, notCompleted

    var title: String {
        switch self {
        case .success: SharedCopy.t("endpoint.successTitle")
        case .timeout: SharedCopy.t("endpoint.timeout")
        case .dns: SharedCopy.t("endpoint.dns")
        case .tls: SharedCopy.t("endpoint.tls")
        case .redirect: SharedCopy.t("endpoint.redirect")
        case .unexpectedResponse: SharedCopy.t("endpoint.unexpectedResponse")
        case .network: SharedCopy.t("endpoint.network")
        case .cancelled: SharedCopy.t("endpoint.cancelled")
        case .notCompleted: SharedCopy.t("endpoint.notCompleted")
        }
    }
}

struct ConnectivityEndpointResult: Sendable {
    let state: ConnectivityEndpointState
    let elapsed: TimeInterval
    /// A fixed portal homepage was requested by a redirect; the target is never retained.
    let campusRedirect: Bool
    /// In-memory selection hint only. A separate fresh campus proof is still required.
    let campusAreaHint: CampusPortalHint?
    init(state: ConnectivityEndpointState, elapsed: TimeInterval, campusRedirect: Bool = false,
         campusAreaHint: CampusPortalHint? = nil) {
        self.state = state
        self.elapsed = elapsed
        self.campusAreaHint = state == .redirect ? campusAreaHint : nil
        self.campusRedirect = state == .redirect && (campusRedirect || campusAreaHint != nil)
    }
    var succeeded: Bool { state == .success }
    var summary: String {
        succeeded ? SharedCopy.f("endpoint.success", ["ms": String(Int((elapsed * 1000).rounded()))]) : state.title
    }
}

struct ConnectivityReport: Sendable {
    let appleResult: ConnectivityEndpointResult
    let microsoftResult: ConnectivityEndpointResult
    let mode: ConnectivityProbeMode
    var apple: Bool { appleResult.succeeded }
    var microsoft: Bool { microsoftResult.succeeded }
    var online: Bool { apple || microsoft }
    var allPassed: Bool { apple && microsoft }
    var passedCount: Int { (apple ? 1 : 0) + (microsoft ? 1 : 0) }
    var resultTitle: String {
        if mode == .complete {
            return allPassed ? SharedCopy.t("env.internet.completeAll") : (online ? SharedCopy.t("env.internet.completePartial") : SharedCopy.t("env.internet.failed"))
        }
        return online ? SharedCopy.t("env.internet.quickPassed") : SharedCopy.t("env.internet.failed")
    }
    var summary: String {
        "Apple：\(appleResult.summary) · Microsoft：\(microsoftResult.summary)"
    }
    var diagnosis: ConnectivityDiagnosis {
        ConnectivityDiagnosis(states: [appleResult.state, microsoftResult.state])
    }

    init(appleResult: ConnectivityEndpointResult, microsoftResult: ConnectivityEndpointResult,
         mode: ConnectivityProbeMode = .quick) {
        self.appleResult = appleResult
        self.microsoftResult = microsoftResult
        self.mode = mode
    }

    /// Preserves source compatibility for existing policy and preview fixtures.
    init(apple: Bool, microsoft: Bool) {
        appleResult = .init(state: apple ? .success : .network, elapsed: 0)
        microsoftResult = .init(state: microsoft ? .success : .network, elapsed: 0)
        mode = .quick
    }
}

/// A plain-language reading of one probe round. It explains a result; it never authorizes
/// anything. The most specific cause wins: a captive redirect usually also breaks HTTPS.
enum ConnectivityDiagnosis: String, Sendable {
    case online, portal, response, security, dns, timeout, unreachable, incomplete

    init(states: [ConnectivityEndpointState]) {
        if states.contains(.success) {
            self = .online
        } else if states.contains(.redirect) {
            self = .portal
        } else if states.contains(.tls) {
            self = .security
        } else if states.contains(.unexpectedResponse) {
            self = .response
        } else if states.contains(.dns) {
            self = .dns
        } else if states.contains(.timeout) {
            self = .timeout
        } else if states.contains(.network) {
            self = .unreachable
        } else {
            self = .incomplete
        }
    }

    var title: String {
        switch self {
        case .online: SharedCopy.t("diagnosis.online.title")
        case .portal: SharedCopy.t("diagnosis.portal.title")
        case .response: SharedCopy.t("diagnosis.response.title")
        case .security: SharedCopy.t("diagnosis.security.title")
        case .dns: SharedCopy.t("diagnosis.dns.title")
        case .timeout: SharedCopy.t("diagnosis.timeout.title")
        case .unreachable: SharedCopy.t("diagnosis.unreachable.title")
        case .incomplete: SharedCopy.t("diagnosis.incomplete.title")
        }
    }

    var explanation: String {
        switch self {
        case .online: SharedCopy.t("diagnosis.online.detail")
        case .portal: SharedCopy.t("diagnosis.portal.detail")
        case .response: SharedCopy.t("diagnosis.response.detail")
        case .security: SharedCopy.t("diagnosis.security.detail")
        case .dns: SharedCopy.t("diagnosis.dns.detail")
        case .timeout: SharedCopy.t("diagnosis.timeout.detail")
        case .unreachable: SharedCopy.t("diagnosis.unreachable.detail")
        case .incomplete: SharedCopy.t("diagnosis.incomplete.detail")
        }
    }
}

/// Pure parsing and payload helpers are separated for offline verification.
enum PortalWire {
    static func object(from data: Data) throws -> [String: Any] {
        guard data.count <= 1_048_576,
              var text = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
        else { throw PortalError.response }
        if !text.hasPrefix("{") {
            if text.hasSuffix(";") { text.removeLast() }
            guard let open = text.firstIndex(of: "("), text.hasSuffix(")") else { throw PortalError.response }
            let callback = String(text[..<open]).trimmingCharacters(in: .whitespaces)
            guard callback == "_" || callback == "jsonpReturn" else { throw PortalError.response }
            text = String(text[text.index(after: open)..<text.index(before: text.endIndex)])
        }
        guard let object = try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
        else { throw PortalError.response }
        return object
    }

    static func teachingSuccess(_ object: [String: Any]) -> Bool {
        if case .accepted = teachingVerdict(object) { return true }
        return false
    }

    static func dormitorySuccess(_ object: [String: Any]) -> Bool {
        let message = object["msg"] as? String ?? ""
        // DrCOM result=1 plus a positive message is a supported success variant.
        // Explicit failure wording must take precedence over the legacy result=0 shortcut.
        guard !isFailureMessage(message) else { return false }
        if isDormitorySuccessMessage(message) { return true }
        if let result = object["result"] as? NSNumber,
           CFGetTypeID(result) != CFBooleanGetTypeID(), result == 0 { return true }
        return false
    }

    enum LoginVerdict {
        case accepted, rejected, uncertain(AuthDiagnostic.Code)
    }

    /// An absent, malformed or contradictory acknowledgement cannot prove a credential rejection.
    static func teachingVerdict(_ object: [String: Any]) -> LoginVerdict {
        // The school's Portal.js treats this as an existing IP session, including in its
        // success callback. It does not prove this request accepted the supplied credentials.
        if ["suc_msg", "error_msg", "error", "res"].contains(where: {
            (object[$0] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) == "ip_already_online_error"
        }) { return .uncertain(.existingSession) }
        var positive = false, negative = false, invalid = false
        for key in ["error", "res"] {
            guard let raw = object[key] else { continue }
            guard let value = raw as? String else { invalid = true; continue }
            if value == "ok" { positive = true }
            else if !value.isEmpty {
                if ["pending", "waiting", "processing"].contains(value.lowercased()) { invalid = true }
                else { negative = true }
            }
        }
        if let raw = object["st"] {
            if let number = raw as? NSNumber {
                if number == 1 { positive = true }
                else if number == 0 { negative = true }
                else { invalid = true }
            } else if let string = raw as? String {
                if string == "1" { positive = true }
                else if string == "0" { negative = true }
                else { invalid = true }
            } else { invalid = true }
        }
        if positive && (negative || invalid) { return .uncertain(.conflictingStatus) }
        if invalid { return .uncertain(.invalidStatus) }
        if positive { return .accepted }
        if negative { return .rejected }
        return .uncertain(.missingStatus)
    }

    static func dormitoryVerdict(_ object: [String: Any]) -> LoginVerdict {
        let rawMessage = object["msg"]
        if rawMessage != nil && !(rawMessage is String) { return .uncertain(.invalidStatus) }
        let message = rawMessage as? String ?? ""
        let explicitFailure = ["失败", "错误", "拒绝", "不存在", "无权限", "未认证成功",
                               "fail", "error", "denied", "invalid", "incorrect", "reject", "not found"]
            .contains { message.lowercased().contains($0) }
        let result = object["result"]
        if let result {
            guard let number = result as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
                  number == 0 || number == 1 else { return .uncertain(.invalidStatus) }
        }
        let successResult = (result as? NSNumber).map {
            CFGetTypeID($0) != CFBooleanGetTypeID() && $0 == 0
        } ?? false
        if explicitFailure && successResult { return .uncertain(.conflictingStatus) }
        if explicitFailure { return .rejected }
        if dormitorySuccess(object) { return .accepted }
        return .uncertain(object["result"] == nil && rawMessage == nil ? .missingStatus : .invalidStatus)
    }

    private static func isFailureMessage(_ raw: String) -> Bool {
        let lower = raw.lowercased()
        return ["未", "不", "非", "无", "没", "失败", "错误", "异常", "拒绝", "等待", "正在", "稍后"].contains { raw.contains($0) }
            || ["fail", "error", "denied", "invalid", "incorrect", "reject", "not ", "pending", "waiting"].contains { lower.contains($0) }
    }

    static func isDormitorySuccessMessage(_ raw: String) -> Bool {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        while let last = text.last, "！!。.~～ ".contains(last) { text.removeLast() }
        return ["认证成功", "portal协议认证成功"].contains(text.lowercased())
    }

    /// Fixed syntax only. Response bytes, controller values and authority are never cached.
    private enum ControllerExpressions {
        static let assignment = try? NSRegularExpression(pattern: #"(?<![A-Za-z0-9_-])(?:ac_id|acid|"(?:ac_id|acid)"|'(?:ac_id|acid)')\s*[:=]\s*(?:"([^"]*)"|'([^']*)'|([^\s&"'<>;,)}\]]*))"#, options: .caseInsensitive)
        static let input = try? NSRegularExpression(pattern: #"<input\b[^>]*>"#, options: .caseInsensitive)
        static let name = try? NSRegularExpression(pattern: #"(?<![A-Za-z0-9_:-])name\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))"#, options: .caseInsensitive)
        static let value = try? NSRegularExpression(pattern: #"(?<![A-Za-z0-9_:-])value\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))"#, options: .caseInsensitive)
    }

    /// Read only the fixed HTTPS page. A missing, invalid or conflicting controller must
    /// stop before credentials are submitted to an assumed controller.
    static func acID(in data: Data) throws -> String {
        guard data.count <= 1_048_576 else { throw PortalError.responseTooLarge }
        guard let html = String(data: data, encoding: .utf8) else { throw PortalError.response }
        guard let assignment = ControllerExpressions.assignment, let inputs = ControllerExpressions.input,
              let name = ControllerExpressions.name, let value = ControllerExpressions.value else { throw PortalError.response }
        func values(_ regex: NSRegularExpression, in text: String) -> [String] {
            return regex.matches(in: text, range: NSRange(text.startIndex..., in: text)).flatMap { match in
                (1..<match.numberOfRanges).compactMap { index in
                    Range(match.range(at: index), in: text).map { String(text[$0]) }
                }
            }
        }
        // The live fixed HTTPS page uses `acid`; legacy pages use `ac_id`. Treat both as
        // one controller and reject invalid or conflicting values before any submission.
        var candidates = values(assignment, in: html)
        for match in inputs.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
            guard let range = Range(match.range, in: html) else { continue }
            let tag = String(html[range])
            let names = values(name, in: tag)
            guard names.contains(where: { ["ac_id", "acid"].contains($0) }) else { continue }
            let ids = values(value, in: tag)
            guard Set(names).count == 1, !ids.isEmpty else { throw PortalError.protocolConflict }
            candidates.append(contentsOf: ids)
        }
        var identifiers = Set<String>()
        for value in candidates {
            guard value.range(of: #"^[0-9]{1,8}$"#, options: .regularExpression) != nil,
                  let number = Int(value), number > 0 else { throw PortalError.protocolConflict }
            identifiers.insert(String(number))
        }
        guard identifiers.count == 1, let identifier = identifiers.first else { throw PortalError.protocolConflict }
        return identifier
    }

    /// Read a single inline CONFIG declaration, never execute JavaScript. Only the
    /// two literal top-level fields needed to associate this page with its source
    /// are returned. Generic page reachability is deliberately insufficient.
    static func teachingClientConfiguration(in data: Data) -> (acID: String, ip: String)? {
        guard data.count <= 1_048_576, let html = String(data: data, encoding: .utf8),
              let acID = try? acID(in: data) else { return nil }
        return SRunClientConfiguration.decode(html, controller: acID)
    }

    /// A challenge is not a final login acknowledgement. Require its own explicit,
    /// type-correct status before generating any password-derived authentication material.
    static func challenge(from object: [String: Any]) throws -> (challenge: String, ip: String) {
        var states: [String] = []
        for key in ["error", "res"] {
            guard let raw = object[key] else { continue }
            guard let state = raw as? String, !state.isEmpty else { throw PortalError.protocolConflict }
            states.append(state)
        }
        guard !states.isEmpty else { throw PortalError.protocolConflict }
        guard states.contains("ok") else { throw PortalError.rejected }
        guard states.allSatisfy({ $0 == "ok" }) else { throw PortalError.protocolConflict }
        guard let challenge = object["challenge"] as? String, !challenge.isEmpty,
              challenge.utf8.count <= 4096,
              let ip = object["client_ip"] as? String, !ip.isEmpty, ip.count <= 64 else { throw PortalError.protocolConflict }
        return (challenge, ip)
    }

    static func isConnectivitySuccess(_ data: Data, status: Int) -> Bool {
        status == 200 && String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
        == "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
    }

    static func isMicrosoftConnectivitySuccess(_ data: Data, status: Int) -> Bool {
        status == 200 && String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
        == "Microsoft Connect Test"
    }

    /// No generic HTTP 200, private address, SSID or redirect is campus evidence.
    /// Require both a campus identity and the known authentication implementation.
    static func isCampusHomepage(_ data: Data, status: Int, url: URL) -> Bool {
        guard status == 200, data.count <= 1_048_576, url.query == nil,
              ["/", "/srun_portal_pc"].contains(url.path), isAllowed(url),
              let html = String(data: data, encoding: .utf8)?.lowercased() else { return false }
        let campus = html.contains("深圳大学") || html.contains("shenzhen university")
        guard campus else { return false }
        if url.host == "net.szu.edu.cn" {
            return html.contains("srun") && (html.contains("login") || html.contains("登录"))
        }
        if url.host == "172.30.255.42" {
            return html.contains("drcom") && (html.contains("login") || html.contains("登录"))
        }
        return false
    }

    /// EPortal is only area evidence after this round established HTTPS campus identity.
    static func isDormitoryHomepage(_ data: Data, status: Int, url: URL, campusConfirmed: Bool) -> Bool {
        guard campusConfirmed, status == 200, data.count <= 1_048_576,
              url.host == "172.30.255.42", url.path == "/eportal", url.query == nil,
              isAllowed(url), let html = String(data: data, encoding: .utf8)?.lowercased()
        else { return false }
        return html.range(of: #"<title>\s*eportal\s*</title>"#, options: .regularExpression) != nil
    }

    static func isAllowed(_ url: URL) -> Bool {
        guard url.user == nil, url.password == nil, url.fragment == nil else { return false }
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

    static func makeURL(base: String, path: String, parameters: [URLQueryItem] = []) throws -> URL {
        guard var components = URLComponents(string: base) else { throw PortalError.forbiddenTarget }
        components.path = path
        if !parameters.isEmpty { components.queryItems = parameters }
        // '+' must be percent-encoded for form-style campus query parsers.
        components.percentEncodedQuery = components.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        guard let url = components.url, isAllowed(url) else { throw PortalError.forbiddenTarget }
        return url
    }

    /// Availability failures that may clear up within a second. Only credential-free reads
    /// that precede any submission are ever repeated because of them.
    static func isTransient(_ error: PortalError) -> Bool {
        switch error {
        case .timeout, .network, .dns: return true
        default: return false
        }
    }

    static func isTLSError(_ error: URLError) -> Bool {
        switch error.code {
        case .secureConnectionFailed, .serverCertificateHasBadDate, .serverCertificateUntrusted,
             .serverCertificateHasUnknownRoot, .serverCertificateNotYetValid,
             .clientCertificateRejected, .clientCertificateRequired, .appTransportSecurityRequiresSecureConnection:
            return true
        default: return false
        }
    }

    static func payload(credentials: Credentials, challenge: String, ip: String, acID: String) throws -> (info: String, hash: String, checksum: String) {
        func quoted(_ value: String) throws -> String {
            let data = try JSONSerialization.data(withJSONObject: value, options: [.fragmentsAllowed, .withoutEscapingSlashes])
            guard let result = String(data: data, encoding: .utf8) else { throw PortalError.cryptography }
            return result
        }
        // Preserve upstream field order; bytes, including UTF-8, participate in XEncode.
        let infoJSON = try "{\"username\":" + quoted(credentials.account)
            + ",\"password\":" + quoted(credentials.password) + ",\"ip\":" + quoted(ip)
            + ",\"acid\":" + quoted(acID) + ",\"enc_ver\":\"srun_bx1\"}"
        let key = Array(challenge.utf8)
        let bytes = Array(infoJSON.utf8)
        let passwordBytes = Array(credentials.password.utf8)
        func take(_ pointer: UnsafeMutablePointer<CChar>?) throws -> String {
            guard let pointer else { throw PortalError.cryptography }
            defer { szu_release(pointer) }
            return String(cString: pointer)
        }
        let encoded = try bytes.withUnsafeBufferPointer { data in
            try key.withUnsafeBufferPointer { keyData in
                try take(szu_xencode_base64(data.baseAddress, data.count, keyData.baseAddress, keyData.count))
            }
        }
        let hash = try passwordBytes.withUnsafeBufferPointer { data in
            try key.withUnsafeBufferPointer { keyData in
                try take(szu_hmac_md5(data.baseAddress, data.count, keyData.baseAddress, keyData.count))
            }
        }
        let info = "{SRBX1}" + encoded
        let checksumBytes = Array(([credentials.account, hash, acID, ip, "200", "1", info]
            .map { challenge + $0 }.joined()).utf8)
        let checksum = try checksumBytes.withUnsafeBufferPointer { try take(szu_sha1($0.baseAddress, $0.count)) }
        return (info, hash, checksum)
    }
}

#if os(macOS)
/// The injection seam keeps physical-route selection and network I/O independently testable.
protocol PortalPhysicalSession: Sendable {
    func data(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse)
    func dataWithDiagnostic(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse)
    func cancel() async
}
extension PortalPhysicalSession {
    func dataWithDiagnostic(for request: URLRequest, timeout: TimeInterval, limit: Int) async throws -> (Data, HTTPURLResponse) {
        try await data(for: request, timeout: timeout, limit: limit)
    }
}
extension CampusPhysicalTransport: PortalPhysicalSession {}
#endif

actor PortalClient {
    private var authenticationSession: URLSession
    private var probeSession: URLSession
    private let protocolClasses: [AnyClass]?
    private var transportGeneration: UInt64 = 0
    private let probeTimeout: TimeInterval
    private let authenticationTimeout: TimeInterval
    private let retryPause: Duration
    #if os(macOS)
    private let physicalSelectionProvider: @Sendable () -> CampusPhysicalSelection
    private let targetedRouteProvider: @Sendable (String) -> CampusPhysicalSelection
    private let physicalTransportFactory: @Sendable (CampusPhysicalRoute, Bool) -> any PortalPhysicalSession
    private var expectedPhysicalRoute: CampusPhysicalRoute?
    private var physicalSelection: CampusPhysicalSelection?
    private var physicalAuthentication: (any PortalPhysicalSession)?
    private var physicalProbe: (any PortalPhysicalSession)?
    #endif

    /// URLProtocol injection exercises the real request pipeline without network access.
    /// The application uses the fixed defaults; timeouts may only be shortened by tests.
    #if os(macOS)
    init(protocolClasses: [AnyClass]? = nil, probeTimeout: TimeInterval = 6,
         authenticationTimeout: TimeInterval = 12,
         physicalSelectionProvider: (@Sendable () -> CampusPhysicalSelection)? = nil,
         targetedRouteProvider: (@Sendable (String) -> CampusPhysicalSelection)? = nil,
         physicalTransportFactory: @escaping @Sendable (CampusPhysicalRoute, Bool) -> any PortalPhysicalSession = {
             CampusPhysicalTransport(route: $0, cookies: $1)
         }) {
        if let physicalSelectionProvider { self.physicalSelectionProvider = physicalSelectionProvider }
        else if protocolClasses == nil { self.physicalSelectionProvider = { CampusPhysicalNetwork.capture() } }
        else { self.physicalSelectionProvider = { .system } }
        if let targetedRouteProvider { self.targetedRouteProvider = targetedRouteProvider }
        else if protocolClasses != nil, let physicalSelectionProvider {
            // Offline routing fixtures provide all network evidence, including targeted leases.
            self.targetedRouteProvider = { _ in physicalSelectionProvider() }
        } else if protocolClasses == nil {
            self.targetedRouteProvider = { CampusPhysicalNetwork.capture(interfaceName: $0) }
        } else { self.targetedRouteProvider = { _ in .unavailable } }
        self.physicalTransportFactory = physicalTransportFactory
        self.probeTimeout = max(0.05, min(probeTimeout, 6))
        self.authenticationTimeout = max(0.05, min(authenticationTimeout, 12))
        self.retryPause = .milliseconds(Int((min(0.8, self.probeTimeout / 4) * 1000).rounded()))
        self.protocolClasses = protocolClasses
        authenticationSession = Self.makeSession(cookies: true, timeout: self.authenticationTimeout, protocolClasses: protocolClasses)
        probeSession = Self.makeSession(cookies: false, timeout: self.probeTimeout, protocolClasses: protocolClasses)
        physicalSelection = self.physicalSelectionProvider()
    }
    #else
    init(protocolClasses: [AnyClass]? = nil, probeTimeout: TimeInterval = 6,
         authenticationTimeout: TimeInterval = 12) {
        self.probeTimeout = max(0.05, min(probeTimeout, 6))
        self.authenticationTimeout = max(0.05, min(authenticationTimeout, 12))
        self.retryPause = .milliseconds(Int((min(0.8, self.probeTimeout / 4) * 1000).rounded()))
        self.protocolClasses = protocolClasses
        // Authentication cookies remain ephemeral and never enter connectivity probes.
        authenticationSession = Self.makeSession(cookies: true, timeout: self.authenticationTimeout, protocolClasses: protocolClasses)
        probeSession = Self.makeSession(cookies: false, timeout: self.probeTimeout, protocolClasses: protocolClasses)
    }
    #endif

    /// Public connectivity is display evidence only. Its private client never enters
    /// campus discovery or authentication, and does not share their route or cookies.
    static func probeSystemConnectivity(mode: ConnectivityProbeMode, protocolClasses: [AnyClass]? = nil) async -> ConnectivityReport {
        let client = PortalClient(systemProbeProtocols: protocolClasses)
        return await client.probe(mode: mode)
    }

    private init(systemProbeProtocols: [AnyClass]?) {
        protocolClasses = systemProbeProtocols
        probeTimeout = 6
        authenticationTimeout = 12
        retryPause = .milliseconds(800)
        #if os(macOS)
        physicalSelectionProvider = { .system }
        targetedRouteProvider = { _ in .unavailable }
        physicalTransportFactory = { CampusPhysicalTransport(route: $0, cookies: $1) }
        physicalSelection = .system
        #endif
        authenticationSession = Self.makeSession(cookies: false, timeout: 6, protocolClasses: systemProbeProtocols)
        probeSession = Self.makeSession(cookies: false, timeout: 6, protocolClasses: systemProbeProtocols, usesSystemProxy: true)
    }

    private static func makeSession(cookies: Bool, timeout: TimeInterval, protocolClasses: [AnyClass]?, usesSystemProxy: Bool = false) -> URLSession {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.urlCache = nil
        if !usesSystemProxy { configuration.connectionProxyDictionary = [:] }
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = min(timeout, cookies ? 8 : 4)
        configuration.timeoutIntervalForResource = timeout
        configuration.waitsForConnectivity = false
        configuration.allowsCellularAccess = false
        configuration.httpMaximumConnectionsPerHost = 2
        configuration.httpShouldSetCookies = cookies
        configuration.httpCookieAcceptPolicy = cookies ? .onlyFromMainDocumentDomain : .never
        if !cookies { configuration.httpCookieStorage = nil }
        if let protocolClasses { configuration.protocolClasses = protocolClasses }
        return URLSession(configuration: configuration, delegate: NoRedirectDelegate(), delegateQueue: nil)
    }

    /// A new network or explicit retry gets fresh ephemeral cookies and connection pools.
    /// Bump the generation before cancelling so suspended operations cannot enter the new sessions.
    func resetConnections() async {
        transportGeneration &+= 1
        #if os(macOS)
        let oldPhysicalAuthentication = physicalAuthentication
        let oldPhysicalProbe = physicalProbe
        physicalAuthentication = nil
        physicalProbe = nil
        // Pin the new route at reset, not midway through discovery or credential submission.
        physicalSelection = expectedPhysicalRoute.map(CampusPhysicalSelection.bound) ?? physicalSelectionProvider()
        #endif
        let previousAuthentication = authenticationSession
        let previousProbe = probeSession
        authenticationSession = Self.makeSession(cookies: true, timeout: authenticationTimeout, protocolClasses: protocolClasses)
        probeSession = Self.makeSession(cookies: false, timeout: probeTimeout, protocolClasses: protocolClasses)
        previousAuthentication.invalidateAndCancel()
        previousProbe.invalidateAndCancel()
        #if os(macOS)
        await oldPhysicalAuthentication?.cancel()
        await oldPhysicalProbe?.cancel()
        #endif
    }

    private func checkGeneration(_ generation: UInt64) throws {
        try Task.checkCancellation()
        guard generation == transportGeneration else { throw CancellationError() }
    }

    /// One full lease capture for the current uninterrupted actor segment.
    private func checkTransport(_ generation: UInt64) throws {
        try checkGeneration(generation)
        #if os(macOS)
        if let expectedPhysicalRoute {
            guard physicalSelection == .bound(expectedPhysicalRoute),
                  targetedRouteProvider(expectedPhysicalRoute.interfaceName) == .bound(expectedPhysicalRoute)
            else { throw CancellationError() }
        } else if let physicalSelection {
            guard physicalSelectionProvider() == physicalSelection else { throw CancellationError() }
        } else { throw CancellationError() }
        #endif
    }

    /// Bind the caller's admitted network identity to its exact live transport lease.
    /// A rejected lease remains pinned so no later probe/reset can silently replace it.
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool {
        #if os(macOS)
        if expectedPhysicalRoute != route {
            expectedPhysicalRoute = route
            await resetConnections()
        }
        guard expectedPhysicalRoute == route else { return false }
        if let route {
            return physicalSelection == .bound(route) && targetedRouteProvider(route.interfaceName) == .bound(route)
        }
        return true
        #else
        return route == nil
        #endif
    }

    private func transportIsCurrent(_ generation: UInt64) -> Bool {
        do { try checkTransport(generation); return true }
        catch { return false }
    }

    deinit {
        authenticationSession.invalidateAndCancel()
        probeSession.invalidateAndCancel()
    }

    func probe() async -> ConnectivityReport { await probe(mode: .quick) }

    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        #if os(macOS)
        // A new credential-free round may recover a temporarily unavailable route lease.
        // Discovery/submission never re-pin midway through their proof/credential sequence.
        if expectedPhysicalRoute == nil, physicalSelectionProvider() != physicalSelection { await resetConnections() }
        #endif
        let generation = transportGeneration
        let report = await withTaskGroup(of: (Bool, ConnectivityEndpointResult).self) { group in
            group.addTask { (true, await self.probeEndpoint(apple: true, generation: generation)) }
            group.addTask { (false, await self.probeEndpoint(apple: false, generation: generation)) }
            var apple = ConnectivityEndpointResult(state: .notCompleted, elapsed: 0)
            var microsoft = ConnectivityEndpointResult(state: .notCompleted, elapsed: 0)
            for await (isApple, result) in group {
                if isApple { apple = result } else { microsoft = result }
                if mode == .quick, result.succeeded || result.campusRedirect { group.cancelAll() }
            }
            if mode == .quick, !Task.isCancelled, apple.succeeded && microsoft.state == .cancelled {
                microsoft = .init(state: .notCompleted, elapsed: microsoft.elapsed)
            }
            if mode == .quick, !Task.isCancelled, microsoft.succeeded && apple.state == .cancelled {
                apple = .init(state: .notCompleted, elapsed: apple.elapsed)
            }
            return ConnectivityReport(appleResult: apple, microsoftResult: microsoft, mode: mode)
        }
        guard transportIsCurrent(generation) else {
            return ConnectivityReport(appleResult: .init(state: .cancelled, elapsed: report.appleResult.elapsed),
                                      microsoftResult: .init(state: .cancelled, elapsed: report.microsoftResult.elapsed), mode: mode)
        }
        return report
    }

    private func probeEndpoint(apple: Bool, generation: UInt64) async -> ConnectivityEndpointResult {
        let started = ContinuousClock.now
        let state: ConnectivityEndpointState
        var campusAreaHint: CampusPortalHint? = nil
        do {
            try Task.checkCancellation()
            let (data, response) = try await get(
                base: apple ? "https://www.apple.com" : "http://www.msftconnecttest.com",
                path: apple ? "/library/test/success.html" : "/connecttest.txt", purpose: .probe, generation: generation)
            let success = apple ? PortalWire.isConnectivitySuccess(data, status: response.statusCode)
                : PortalWire.isMicrosoftConnectivitySuccess(data, status: response.statusCode)
            state = success ? .success : .unexpectedResponse
        } catch is CancellationError {
            state = .cancelled
        } catch ProbeRedirect.campus(let hint) {
            state = .redirect
            campusAreaHint = hint
        } catch let error as PortalError {
            switch error {
            case .timeout: state = .timeout
            case .dns: state = .dns
            case .tls: state = .tls
            case .redirect: state = .redirect
            case .response, .httpStatus, .responseTooLarge: state = .unexpectedResponse
            default: state = .network
            }
        } catch { state = .network }
        let duration = started.duration(to: .now).components
        let elapsed = Double(duration.seconds) + Double(duration.attoseconds) / 1e18
        return ConnectivityEndpointResult(state: state, elapsed: max(0, elapsed), campusAreaHint: campusAreaHint)
    }

    /// Read-only, credential-free and bounded; identity is never cached across paths.
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        let generation = transportGeneration
        let discovery = await discoverCampus(area: area)
        guard (try? checkGeneration(generation)) != nil else { return nil }
        return discovery.confirmedCandidate(for: area)
    }

    /// Compatibility facade; the classified path owns every protocol request and verdict.
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        try Task.checkCancellation()
        let generation = transportGeneration
        var target: CampusArea = area == .automatic ? .teaching : area
        var attempt = await attemptLogin(credentials: credentials, area: target)
        try checkGeneration(generation)
        if area == .automatic, attempt.submissionNotStarted {
            let discovery = await discoverCampus(area: .dormitory)
            try checkGeneration(generation)
            if discovery.teachingConfirmed && discovery.dormitoryConfirmed {
                target = .dormitory
                attempt = await attemptLogin(credentials: credentials, area: target)
                try checkGeneration(generation)
            }
        }
        switch attempt.outcome {
        case .success:
            return attempt.message ?? SharedCopy.t(target == .dormitory ? "event.succeededDormitory" : "event.succeededTeaching")
        case .cancelled: throw CancellationError()
        case .invalidCredentials: throw PortalError.invalidCredentials
        case .uncertain: throw PortalError.authenticationUncertain
        case .rejected, .challengeRefused: throw attempt.error ?? PortalError.rejected
        case .security: throw attempt.error ?? PortalError.protocolConflict
        case .unavailable: throw attempt.error ?? PortalError.network
        }
    }

    private enum RequestPurpose { case authentication, authenticationDiscovery, discovery, probe }
    private enum ProbeRedirect: Error { case campus(CampusPortalHint) }

    /// Every asynchronous transport boundary gets exactly one fresh post-await lease check,
    /// whether it returned a reply or an error. Parsing afterwards cannot suspend the actor.
    private func checkedTransfer<T: Sendable>(generation: UInt64,
                                              _ operation: () async throws -> T) async throws -> T {
        let result: Result<T, Error>
        do { result = .success(try await operation()) }
        catch { result = .failure(error) }
        try checkTransport(generation)
        return try result.get()
    }

    private static func checkResponse(_ response: HTTPURLResponse, url: URL, isProbe: Bool, limit: Int) throws {
        guard response.url == url else { throw PortalError.redirect }
        if (300..<400).contains(response.statusCode) {
            if isProbe, let hint = CampusRedirectTarget.areaHint(response.value(forHTTPHeaderField: "Location"), relativeTo: url) {
                throw ProbeRedirect.campus(hint)
            }
            throw PortalError.redirect
        }
        guard response.statusCode == 200 else { throw PortalError.httpStatus }
        guard response.expectedContentLength <= Int64(limit) else { throw PortalError.responseTooLarge }
    }

    #if os(macOS)
    private func physicalSession(purpose: RequestPurpose, generation: UInt64) throws -> (any PortalPhysicalSession)? {
        try checkTransport(generation)
        guard let pinned = physicalSelection else { throw CancellationError() }
        switch pinned {
        case .system: return nil
        case .unavailable: throw PortalError.network
        case .bound(let route):
            if purpose == .authentication || purpose == .authenticationDiscovery {
                if physicalAuthentication == nil { physicalAuthentication = physicalTransportFactory(route, true) }
                return physicalAuthentication
            }
            if physicalProbe == nil { physicalProbe = physicalTransportFactory(route, false) }
            return physicalProbe
        }
    }

    private static func physicalError(_ error: CampusPhysicalTransportError) -> Error {
        switch error {
        case .cancelled: return CancellationError()
        case .timeout: return PortalError.timeout
        case .dns: return PortalError.dns
        case .tls: return PortalError.tls
        case .network: return PortalError.network
        case .response: return PortalError.response
        case .responseTooLarge: return PortalError.responseTooLarge
        }
    }
    #endif

    /// Credential-free reads that precede any submission may be repeated once after a short
    /// pause when the failure looks transient. Submissions never pass through here.
    private struct DiscoveryFailure: Error {
        let issue: CampusVerificationIssue
        let underlying: PortalError
    }
    private func idempotentGet(base: String, path: String, parameters: [URLQueryItem] = [],
                               purpose: RequestPurpose, generation: UInt64) async throws -> (Data, HTTPURLResponse) {
        do {
            return try await get(base: base, path: path, parameters: parameters, purpose: purpose, generation: generation)
        } catch let error as DiscoveryFailure where [.dnsTimeout, .dns, .connection].contains(error.issue) && PortalWire.isTransient(error.underlying) {
            try await Task.sleep(for: retryPause)
            return try await get(base: base, path: path, parameters: parameters, purpose: purpose, generation: generation)
        } catch let error as PortalError where PortalWire.isTransient(error) {
            try await Task.sleep(for: retryPause)
            return try await get(base: base, path: path, parameters: parameters, purpose: purpose, generation: generation)
        }
    }

    private func get(base: String, path: String, parameters: [URLQueryItem] = [],
                     purpose: RequestPurpose = .authentication, generation: UInt64) async throws -> (Data, HTTPURLResponse) {
        try checkGeneration(generation)
        let url = try PortalWire.makeURL(base: base, path: path, parameters: parameters)
        let isProbe = purpose == .probe
        // Read-only identity discovery is isolated; login discovery keeps the authentication cookie chain.
        let session = purpose == .authentication || purpose == .authenticationDiscovery ? authenticationSession : probeSession
        let timeout = purpose == .authentication ? authenticationTimeout : probeTimeout
        let limit = isProbe ? 65_536 : 1_048_576
        var request = URLRequest(url: url)
        request.mainDocumentURL = URL(string: base + "/")
        request.timeoutInterval = timeout
        request.setValue("no-store", forHTTPHeaderField: "Cache-Control")
        do {
            #if os(macOS)
            if let physical = try physicalSession(purpose: purpose, generation: generation) {
                let (data, response): (Data, HTTPURLResponse)
                do {
                    (data, response) = try await checkedTransfer(generation: generation) {
                        try await physical.dataWithDiagnostic(for: request, timeout: timeout, limit: limit)
                    }
                } catch let error as CampusPhysicalDiagnosticError {
                    if purpose == .discovery {
                        if error.diagnostic.reason == .cancelled { throw CancellationError() }
                        let issue: CampusVerificationIssue
                        switch error.diagnostic.reason {
                        case .resolutionTimeout: issue = .dnsTimeout
                        case .resolutionFailed: issue = .dns
                        case .dnsTlsFailed: issue = .dnsTls
                        case .tlsFailed: issue = .tls
                        case .bindingChanged: issue = .binding
                        case .httpRejected: issue = .http
                        case .malformedResponse, .responseTooLarge: issue = .page
                        default: issue = .connection
                        }
                        throw DiscoveryFailure(issue: issue, underlying: Self.physicalError(error.underlying) as? PortalError ?? .network)
                    }
                    throw Self.physicalError(error.underlying)
                } catch let error as CampusPhysicalTransportError { throw Self.physicalError(error) }
                try Self.checkResponse(response, url: url, isProbe: isProbe, limit: limit)
                guard data.count <= limit else { throw PortalError.responseTooLarge }
                return (data, response)
            }
            #endif
            let result = try await checkedTransfer(generation: generation) {
                try await withThrowingTaskGroup(of: (Data, HTTPURLResponse).self) { group in
                    group.addTask {
                        let (bytes, rawResponse) = try await session.bytes(for: request)
                        defer { bytes.task.cancel() }
                        try Task.checkCancellation()
                        guard let response = rawResponse as? HTTPURLResponse else { throw PortalError.response }
                        try Self.checkResponse(response, url: url, isProbe: isProbe, limit: limit)
                        return try await withTaskCancellationHandler {
                            var data = Data()
                            data.reserveCapacity(min(limit, 4096))
                            // Preserve per-byte limits/cancellation; append to Foundation Data in small
                            // blocks instead of invoking its mutation path for every response byte.
                            var chunk: [UInt8] = []
                            chunk.reserveCapacity(4096)
                            for try await byte in bytes {
                                try Task.checkCancellation()
                                guard data.count + chunk.count < limit else { throw PortalError.responseTooLarge }
                                chunk.append(byte)
                                if chunk.count == 4096 {
                                    data.append(contentsOf: chunk)
                                    chunk.removeAll(keepingCapacity: true)
                                }
                            }
                            try Task.checkCancellation()
                            data.append(contentsOf: chunk)
                            return (data, response)
                        } onCancel: { bytes.task.cancel() }
                    }
                    group.addTask {
                        try await Task.sleep(for: .seconds(timeout))
                        throw PortalError.timeout
                    }
                    defer { group.cancelAll() }
                    guard let result = try await group.next() else { throw CancellationError() }
                    return result
                }
            }
            return result
        } catch is CancellationError {
            throw CancellationError()
        } catch let error as ProbeRedirect {
            try checkGeneration(generation)
            throw error
        } catch let error as DiscoveryFailure {
            try checkGeneration(generation)
            throw error
        } catch let error as PortalError {
            try checkGeneration(generation)
            throw error
        } catch {
            try checkGeneration(generation)
            if let urlError = error as? URLError {
                if urlError.code == .cancelled { throw CancellationError() }
                if PortalWire.isTLSError(urlError) { throw PortalError.tls }
                switch urlError.code {
                case .timedOut: throw PortalError.timeout
                case .cannotFindHost, .dnsLookupFailed: throw PortalError.dns
                default: break
                }
            }
            // Never expose URLError.userInfo, request URLs, or raw server bodies.
            throw PortalError.network
        }
    }
}

// MARK: - Staged discovery and classified single attempts

extension PortalClient {
    private static let operatingSystemName: String = {
        #if os(macOS)
        return "macOS"
        #else
        return "iOS"
        #endif
    }()

    /// Read-only and credential-free. Any HTTPS identity failure (including TLS) stops before the
    /// plaintext dormitory page, exactly like `confirmedCampusArea`.
    private static func verificationIssue(_ error: Error) -> CampusVerificationIssue {
        if let failure = error as? DiscoveryFailure { return failure.issue }
        guard let portal = error as? PortalError else { return error is CancellationError ? .none : .connection }
        switch portal {
        case .dns: return .dns
        case .tls, .cryptography, .forbiddenTarget: return .tls
        case .httpStatus, .redirect: return .http
        case .response, .responseTooLarge, .protocolConflict: return .page
        default: return .connection
        }
    }

    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        let generation = transportGeneration
        var result = CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
        guard !Task.isCancelled else { return result }
        do {
            let (data, response) = try await idempotentGet(base: "https://net.szu.edu.cn", path: "/srun_portal_pc", purpose: .discovery, generation: generation)
            guard let url = response.url, PortalWire.isCampusHomepage(data, status: response.statusCode, url: url) else {
                result.issue = .page
                return result
            }
            if let configuration = PortalWire.teachingClientConfiguration(in: data) {
                result.teachingClientBound = configuration.ip == boundSourceAddress
            }
        } catch {
            result.issue = Self.verificationIssue(error)
            return result
        }
        guard (try? checkGeneration(generation)) != nil else { return result }
        result.teaching = .confirmed
        if area == .teaching || Task.isCancelled { return result }
        do {
            let (data, response) = try await idempotentGet(base: "http://172.30.255.42:801", path: "/eportal", purpose: .discovery, generation: generation)
            if let url = response.url,
               PortalWire.isDormitoryHomepage(data, status: response.statusCode, url: url, campusConfirmed: true) {
                result.dormitory = .confirmed
            } else {
                result.dormitory = .unconfirmed
                result.issue = .page
            }
        } catch {
            if error is CancellationError {
                return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
            }
            result.dormitory = .unconfirmed
            result.issue = Self.verificationIssue(error)
        }
        guard (try? checkGeneration(generation)) != nil else { return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked) }
        return result
    }

    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        await attemptLogin(credentials: credentials, area: area, requireBoundClientConfiguration: false)
    }

    func attemptLogin(credentials: Credentials, area: CampusArea, requireBoundClientConfiguration: Bool) async -> LoginAttempt {
        guard !credentials.account.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              !credentials.password.isEmpty, credentials.account.utf8.count <= 256,
              credentials.password.utf8.count <= 1024 else { return LoginAttempt(error: PortalError.invalidCredentials) }
        if Task.isCancelled { return .cancelled }
        let generation = transportGeneration
        let result = area == .dormitory ? await dormitoryAttempt(credentials, generation: generation)
            : await teachingAttempt(credentials, generation: generation, requireBoundClientConfiguration: requireBoundClientConfiguration)
        return (try? checkGeneration(generation)) != nil ? result : .cancelled
    }

    /// After a credential-bearing request started, availability/parse failures are "uncertain".
    private static func afterSubmission(_ error: Error) -> Error {
        guard let portal = error as? PortalError else { return PortalError.authenticationUncertain }
        switch portal {
        case .network, .timeout, .dns, .httpStatus, .response, .responseTooLarge, .redirect: return PortalError.authenticationUncertain
        default: return portal
        }
    }

    private static func diagnosticCode(for error: Error) -> AuthDiagnostic.Code {
        guard let error = error as? PortalError else { return .transportFailure }
        switch error {
        case .httpStatus: return .httpFailure
        case .tls, .forbiddenTarget, .cryptography, .redirect, .protocolConflict: return .securityFailure
        case .response, .responseTooLarge: return .malformedResponse
        case .rejected: return .explicitRejection
        default: return .transportFailure
        }
    }

    private static func diagnosed(_ attempt: LoginAttempt, stage: AuthDiagnostic.Stage,
                                  path: AuthDiagnostic.Path, acID: String? = nil,
                                  status: Int? = nil, code: AuthDiagnostic.Code,
                                  serverCode: AuthDiagnostic.ServerCode? = nil) -> LoginAttempt {
        var result = attempt
        result.diagnostic = AuthDiagnostic(stage: stage, path: path, httpStatus: status, acID: acID,
                                           code: code, serverCode: serverCode)
        return result
    }

    private var boundSourceAddress: String? {
        #if os(macOS)
        guard case .bound(let route) = physicalSelection else { return nil }
        return route.addresses.first
        #else
        return nil
        #endif
    }

    private func teachingAttempt(_ credentials: Credentials, generation: UInt64,
                                 requireBoundClientConfiguration: Bool) async -> LoginAttempt {
        let base = "https://net.szu.edu.cn"
        let acID: String
        do {
            let (data, response) = try await idempotentGet(base: base, path: "/srun_portal_pc", purpose: .authenticationDiscovery, generation: generation)
            if requireBoundClientConfiguration {
                guard let url = response.url, PortalWire.isCampusHomepage(data, status: response.statusCode, url: url),
                      let configuration = PortalWire.teachingClientConfiguration(in: data),
                      configuration.ip == boundSourceAddress else { throw PortalError.protocolConflict }
                // This configuration already validated the controller against this exact response.
                acID = configuration.acID
            } else {
                acID = try PortalWire.acID(in: data)
            }
        } catch {
            if Task.isCancelled || error is CancellationError { return .cancelled }
            return Self.diagnosed(LoginAttempt(error: error), stage: .homepage, path: .homepage,
                                  code: Self.diagnosticCode(for: error))
        }
        if Task.isCancelled { return .cancelled }
        let challengeData: Data
        do {
            challengeData = try await idempotentGet(base: base, path: "/cgi-bin/get_challenge", parameters: [
                URLQueryItem(name: "callback", value: "_"), URLQueryItem(name: "username", value: credentials.account),
                URLQueryItem(name: "ip", value: "")
            ], purpose: .authentication, generation: generation).0
        } catch {
            if Task.isCancelled || error is CancellationError { return .cancelled }
            return Self.diagnosed(LoginAttempt(error: error), stage: .challenge, path: .challenge, acID: acID,
                                  code: Self.diagnosticCode(for: error))
        }
        let challengeObject: [String: Any]
        do { challengeObject = try PortalWire.object(from: challengeData) }
        catch { return Self.diagnosed(LoginAttempt(error: PortalError.protocolConflict), stage: .challenge,
                                      path: .challenge, acID: acID, status: 200, code: .malformedResponse) }
        let issued: (challenge: String, ip: String)
        do {
            issued = try PortalWire.challenge(from: challengeObject)
            if requireBoundClientConfiguration, issued.ip != boundSourceAddress { throw PortalError.protocolConflict }
        } catch let error as PortalError where error == .rejected {
            // No challenge was issued and nothing was submitted: not a verdict about the account.
            return Self.diagnosed(LoginAttempt(outcome: .challengeRefused, error: .rejected,
                                               message: SharedCopy.t("portalError.challengeRefused")),
                                  stage: .challenge, path: .challenge, acID: acID, status: 200, code: .explicitRejection)
        } catch {
            return Self.diagnosed(LoginAttempt(error: error), stage: .challenge, path: .challenge,
                                  acID: acID, status: 200, code: Self.diagnosticCode(for: error))
        }
        let payload: (info: String, hash: String, checksum: String)
        do { payload = try PortalWire.payload(credentials: credentials, challenge: issued.challenge, ip: issued.ip, acID: acID) }
        catch { return Self.diagnosed(LoginAttempt(error: error), stage: .challenge, path: .challenge,
                                      acID: acID, status: 200, code: Self.diagnosticCode(for: error)) }
        let os = Self.operatingSystemName
        let values = ["callback": "_", "action": "login", "username": credentials.account,
                      "password": "{MD5}" + payload.hash, "os": os, "name": os, "double_stack": "0",
                      "info": payload.info, "chksum": payload.checksum, "ac_id": acID,
                      "ip": issued.ip, "n": "200", "type": "1"]
        if Task.isCancelled { return .cancelled }
        do {
            let (data, response) = try await get(base: base, path: "/cgi-bin/srun_portal",
                parameters: values.sorted { $0.key < $1.key }.map { URLQueryItem(name: $0.key, value: $0.value) }, generation: generation)
            guard let object = try? PortalWire.object(from: data) else {
                return Self.diagnosed(LoginAttempt(error: PortalError.authenticationUncertain), stage: .submission,
                                      path: .teachingSubmit, acID: acID, status: response.statusCode, code: .malformedResponse)
            }
            let attempt: LoginAttempt
            let code: AuthDiagnostic.Code
            switch PortalWire.teachingVerdict(object) {
            case .accepted:
                attempt = .success(SharedCopy.t("event.succeededTeaching")); code = .accepted
            case .rejected:
                attempt = LoginAttempt(outcome: .rejected, rejection: PortalWire.rejectionKind(object), error: .rejected,
                                       message: PortalError.rejected.localizedDescription)
                code = .explicitRejection
            case .uncertain(let reason):
                attempt = LoginAttempt(error: PortalError.authenticationUncertain); code = reason
            }
            return Self.diagnosed(attempt, stage: .submission, path: .teachingSubmit, acID: acID,
                                  status: response.statusCode, code: code,
                                  serverCode: code == .accepted ? nil : PortalWire.serverCode(object))
        } catch {
            if Task.isCancelled || error is CancellationError { return .cancelled }
            return Self.diagnosed(LoginAttempt(error: Self.afterSubmission(error)), stage: .submission,
                                  path: .teachingSubmit, acID: acID, code: Self.diagnosticCode(for: error))
        }
    }

    private func dormitoryAttempt(_ credentials: Credentials, generation: UInt64) async -> LoginAttempt {
        do {
            let (data, response) = try await get(base: "http://172.30.255.42:801", path: "/eportal/portal/login", parameters: [
                URLQueryItem(name: "login_method", value: "1"),
                URLQueryItem(name: "user_account", value: ",0," + credentials.account),
                URLQueryItem(name: "user_password", value: credentials.password)
            ], generation: generation)
            guard let object = try? PortalWire.object(from: data) else {
                return Self.diagnosed(LoginAttempt(error: PortalError.authenticationUncertain), stage: .submission,
                                      path: .dormitorySubmit, status: response.statusCode, code: .malformedResponse)
            }
            let attempt: LoginAttempt
            let code: AuthDiagnostic.Code
            switch PortalWire.dormitoryVerdict(object) {
            case .accepted:
                attempt = .success(SharedCopy.t("event.succeededDormitory")); code = .accepted
            case .rejected:
                attempt = LoginAttempt(outcome: .rejected, rejection: PortalWire.rejectionKind(object), error: .rejected,
                                       message: PortalError.rejected.localizedDescription)
                code = .explicitRejection
            case .uncertain(let reason):
                attempt = LoginAttempt(error: PortalError.authenticationUncertain); code = reason
            }
            return Self.diagnosed(attempt, stage: .submission, path: .dormitorySubmit,
                                  status: response.statusCode, code: code,
                                  serverCode: code == .accepted ? nil : PortalWire.serverCode(object))
        } catch {
            if Task.isCancelled || error is CancellationError { return .cancelled }
            return Self.diagnosed(LoginAttempt(error: Self.afterSubmission(error)), stage: .submission,
                                  path: .dormitorySubmit, code: Self.diagnosticCode(for: error))
        }
    }
}

/// A bounded reader for the fixed page's literal CONFIG object. It skips unrelated
/// JavaScript values without evaluating them; ambiguous syntax yields no candidate.
private enum SRunClientConfiguration {
    private enum Token: Equatable {
        case word(String, escaped: Bool = false), string(String, escaped: Bool), punctuation(UInt8)

        func names(_ text: String) -> Bool {
            if case .word(let value, _) = self { return value == text }
            return false
        }
    }

    static func decode(_ html: String, controller: String) -> (acID: String, ip: String)? {
        var reader = InlineScriptReader(html)
        guard let scripts = reader.read() else { return nil }
        var declaration: [Token]?
        for script in scripts {
            guard let tokens = tokenize(script.body), !computedConfigurationReference(tokens) else { return nil }
            guard tokens.contains(where: { $0.names("CONFIG") }) else { continue }
            guard script.attributes.range(of: #"^\s*(?:type\s*=\s*["'](?:text|application)/(?:java|ecma)script["']\s*)?$"#,
                options: [.regularExpression, .caseInsensitive]) != nil else { return nil }
            // Reject reassignment, property mutation, multiple declarations and
            // CONFIG-like strings/comments rather than trying to interpret code.
            guard declaration == nil, tokens.count >= 5,
                  ["var", "let", "const"].contains(where: { tokens[0] == .word($0) }),
                  tokens[1] == .word("CONFIG"), tokens[2] == .punctuation(61),
                  tokens[3] == .punctuation(123),
                  tokens.filter({ $0.names("CONFIG") }).count == 1 else { return nil }
            declaration = tokens
        }
        guard let tokens = declaration, let fields = objectFields(tokens),
              let ipTokens = fields["ip"], ipTokens.count == 1,
              case .string(let ip, escaped: false) = ipTokens[0], validIPv4(ip) else { return nil }
        let controllers = [fields["acid"], fields["ac_id"]].compactMap { $0 }
        guard controllers.count == 1, let value = controllers.first, value.count == 1 else { return nil }
        let raw: String
        switch value[0] {
        case .word(let text, escaped: false), .string(let text, escaped: false): raw = text
        default: return nil
        }
        guard (1...8).contains(raw.utf8.count), raw.utf8.allSatisfy({ (48...57).contains($0) }),
              let number = Int(raw), number > 0, String(number) == controller else { return nil }
        return (controller, ip)
    }

    /// This deliberately bounded HTML reader is not a browser DOM or a JavaScript
    /// interpreter. Attribute text and inert/raw-text elements cannot declare CONFIG.
    /// Script bodies are sliced from the original bytes, including comment-like strings.
    private struct InlineScriptReader {
        let bytes: [UInt8]
        let lower: [UInt8]
        var index = 0

        init(_ html: String) {
            bytes = Array(html.utf8)
            lower = bytes.map { (65...90).contains($0) ? $0 + 32 : $0 }
        }

        private func space(_ byte: UInt8) -> Bool { [9, 10, 12, 13, 32].contains(byte) }
        private func matches(_ text: String, at start: Int) -> Bool {
            let pattern = Array(text.utf8)
            return start + pattern.count <= lower.count && lower[start..<(start + pattern.count)].elementsEqual(pattern)
        }
        private func find(_ text: String, from start: Int, before end: Int? = nil) -> Int? {
            let pattern = Array(text.utf8)
            let limit = min(end ?? lower.count, lower.count) - pattern.count
            guard start <= limit else { return nil }
            for offset in start...limit where lower[offset] == pattern[0] {
                if lower[offset..<(offset + pattern.count)].elementsEqual(pattern) { return offset }
            }
            return nil
        }
        private func tagEnd(from start: Int) -> Int? {
            var quote: UInt8?
            for offset in start..<bytes.count {
                let byte = bytes[offset]
                if let quoted = quote {
                    if byte == quoted { quote = nil }
                } else if byte == 34 || byte == 39 { quote = byte }
                else if byte == 62 { return offset }
                else if byte == 60 || byte == 0 { return nil }
            }
            return nil
        }
        private func closeTag(_ name: String, from start: Int) -> (start: Int, end: Int)? {
            let prefix = "</" + name
            var cursor = start
            while let open = find(prefix, from: cursor) {
                let after = open + prefix.utf8.count
                guard after < bytes.count else { return nil }
                if space(bytes[after]) || bytes[after] == 62 {
                    var end = after
                    while end < bytes.count, space(bytes[end]) { end += 1 }
                    guard end < bytes.count, bytes[end] == 62 else { return nil }
                    return (open, end)
                }
                cursor = after
            }
            return nil
        }

        mutating func read() -> [(attributes: String, body: String)]? {
            var scripts: [(attributes: String, body: String)] = []
            var templateDepth = 0
            var foreignElements: [String] = []
            let rawText = ["style", "title", "textarea", "xmp", "iframe", "noembed", "noframes", "noscript"]
            while let open = find("<", from: index) {
                if matches("<!--", at: open) {
                    guard let end = find("-->", from: open + 4) else { return nil }
                    index = end + 3
                    continue
                }
                var nameStart = open + 1
                guard nameStart < bytes.count else { break }
                if bytes[nameStart] == 33 || bytes[nameStart] == 63 {
                    // Declarations and processing-instruction-like markup cannot
                    // contribute scripts hidden inside their quoted text.
                    guard let end = tagEnd(from: nameStart + 1) else { return nil }
                    index = end + 1
                    continue
                }
                let closing = bytes[nameStart] == 47
                if closing { nameStart += 1 }
                guard nameStart < bytes.count, (97...122).contains(lower[nameStart]) else {
                    index = nameStart
                    continue
                }
                var afterName = nameStart
                while afterName < bytes.count, !space(bytes[afterName]), ![47, 62].contains(bytes[afterName]) {
                    guard bytes[afterName] != 60, bytes[afterName] != 0 else { return nil }
                    afterName += 1
                }
                let name = String(decoding: lower[nameStart..<afterName], as: UTF8.self)
                guard let end = tagEnd(from: afterName) else { return nil }
                index = end + 1
                // Frameset insertion modes and foreign-content script semantics
                // are outside this HTML literal subset. Ordinary SVG/MathML
                // without scripts may coexist with a normal inline declaration.
                if name == "frameset", templateDepth == 0 { return nil }
                if name == "svg" || name == "math" {
                    if closing {
                        guard foreignElements.popLast() == name else { return nil }
                    } else if bytes[end - 1] != 47 {
                        foreignElements.append(name)
                        guard foreignElements.count <= 32 else { return nil }
                    }
                }
                if !closing, name == "plaintext" { break }
                if name == "template" {
                    templateDepth += closing ? -1 : 1
                    guard (0...32).contains(templateDepth) else { return nil }
                }
                guard !closing, name == "script" || rawText.contains(name) else { continue }
                if name == "script", templateDepth == 0, !foreignElements.isEmpty { return nil }
                guard let close = closeTag(name, from: index) else { return nil }
                // Legacy HTML script escaping can make an apparent </script>
                // stay inside double-escaped raw text. Do not interpret that
                // browser-specific syntax as a second active declaration.
                if name == "script", find("<!--", from: index, before: close.start) != nil { return nil }
                if name == "script", templateDepth == 0 {
                    scripts.append((String(decoding: bytes[afterName..<end], as: UTF8.self),
                                    String(decoding: bytes[index..<close.start], as: UTF8.self)))
                }
                index = close.end + 1
            }
            return templateDepth == 0 && foreignElements.isEmpty ? scripts : nil
        }
    }

    /// Reject visible computed CONFIG access without evaluating keys. A static
    /// distinct global property and ordinary local array indexing remain compatible.
    private static func computedConfigurationReference(_ tokens: [Token]) -> Bool {
        func globalReceiver(_ end: Int, depth: Int = 0) -> Int? {
            guard end >= 0, depth <= 32 else { return nil }
            if end >= 1, tokens[end] == .punctuation(46), tokens[end - 1] == .punctuation(63) {
                return globalReceiver(end - 2, depth: depth + 1)
            }
            if case .word(let name, _) = tokens[end] {
                if ["window", "globalThis", "self", "this"].contains(name) { return end }
                if end >= 2, tokens[end - 1] == .punctuation(46) {
                    let receiver = tokens[end - 2] == .punctuation(63) ? end - 3 : end - 2
                    return globalReceiver(receiver, depth: depth + 1)
                }
            }
            if tokens[end] == .punctuation(41) || tokens[end] == .punctuation(93) {
                let close: UInt8 = tokens[end] == .punctuation(41) ? 41 : 93
                let open: UInt8 = close == 41 ? 40 : 91
                var nesting = 1
                var start = end - 1
                while start >= 0 {
                    if tokens[start] == .punctuation(close) { nesting += 1 }
                    if tokens[start] == .punctuation(open) { nesting -= 1 }
                    if nesting == 0 { break }
                    start -= 1
                }
                guard start >= 0 else { return nil }
                if close == 41, let root = globalReceiver(end - 1, depth: depth + 1), root == start + 1 { return start }
                if close == 93 { return globalReceiver(start - 1, depth: depth + 1) }
            }
            return nil
        }
        for index in tokens.indices where tokens[index] == .punctuation(91) && index > 0 {
            var receiver = index - 1
            if receiver >= 1, tokens[receiver] == .punctuation(46), tokens[receiver - 1] == .punctuation(63) { receiver -= 2 }
            guard receiver >= 0 else { continue }
            switch tokens[receiver] {
            case .word(let text, _):
                if ["return", "throw", "yield"].contains(text) { continue }
            case .string, .punctuation(41), .punctuation(93): break
            default: continue
            }
            var key = ""
            var cursor = index + 1
            var known = false
            while cursor < tokens.count, cursor - index <= 32 {
                guard case .string(let text, let escaped) = tokens[cursor] else { break }
                if escaped { return true }
                key += text
                cursor += 1
                if cursor < tokens.count, tokens[cursor] == .punctuation(93) { known = true; break }
                guard cursor < tokens.count, tokens[cursor] == .punctuation(43) else { break }
                cursor += 1
            }
            if known && key == "CONFIG" { return true }
            if !known && globalReceiver(receiver) != nil { return true }
        }
        return false
    }

    private static func validIPv4(_ text: String) -> Bool {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        return parts.count == 4 && parts.allSatisfy {
            (1...3).contains($0.utf8.count) && $0.utf8.allSatisfy({ (48...57).contains($0) })
                && ($0.count == 1 || $0.first != "0") && (Int($0).map { $0 <= 255 } ?? false)
        }
    }

    private static func literalKey(_ token: Token) -> String? {
        switch token {
        case .string(let text, escaped: false): return text
        case .word(let text, escaped: false):
            // Unquoted keys must be identifiers or supported decimal integers.
            guard let first = text.utf8.first else { return nil }
            if (48...57).contains(first) {
                return text.utf8.allSatisfy({ (48...57).contains($0) })
                    && (text.count == 1 || first != 48) ? text : nil
            }
            return text.range(of: #"^[A-Za-z_$][A-Za-z0-9_$]*$"#, options: .regularExpression) != nil ? text : nil
        default: return nil
        }
    }

    private static func objectFields(_ tokens: [Token]) -> [String: [Token]]? {
        var fields: [String: [Token]] = [:]
        var index = 4
        while index < tokens.count, tokens[index] != .punctuation(125) {
            guard let key = literalKey(tokens[index]) else { return nil }
            guard fields[key] == nil, index + 1 < tokens.count, tokens[index + 1] == .punctuation(58) else { return nil }
            index += 2
            let start = index
            var closing: [UInt8] = []
            while index < tokens.count {
                if case .punctuation(let byte) = tokens[index] {
                    if closing.isEmpty && (byte == 44 || byte == 125) { break }
                    switch byte {
                    case 123: closing.append(125)
                    case 91: closing.append(93)
                    case 40: closing.append(41)
                    case 125, 93, 41:
                        guard closing.popLast() == byte else { return nil }
                    default: break
                    }
                    guard closing.count <= 32 else { return nil }
                }
                index += 1
            }
            guard index > start, closing.isEmpty, fields.count < 256 else { return nil }
            let value = Array(tokens[start..<index])
            var reader = LiteralReader(tokens: value)
            guard reader.value(), reader.index == value.count else { return nil }
            fields[key] = value
            if index < tokens.count, tokens[index] == .punctuation(44) { index += 1 }
            else { break }
        }
        guard index < tokens.count, tokens[index] == .punctuation(125) else { return nil }
        index += 1
        if index < tokens.count, tokens[index] == .punctuation(59) { index += 1 }
        return index == tokens.count ? fields : nil
    }

    /// Validate complete literal values, not just a whitelist of individual tokens.
    /// In particular, nested duplicates, array holes and expression side effects
    /// cannot be hidden in an otherwise unused setting.
    private struct LiteralReader {
        let tokens: [Token]
        var index = 0

        mutating func take(_ byte: UInt8) -> Bool {
            guard index < tokens.count, tokens[index] == .punctuation(byte) else { return false }
            index += 1
            return true
        }

        mutating func value(depth: Int = 0) -> Bool {
            guard depth <= 32, index < tokens.count else { return false }
            if take(123) {
                var keys = Set<String>()
                if take(125) { return true }
                while index < tokens.count {
                    guard let key = literalKey(tokens[index]) else { return false }
                    index += 1
                    guard keys.insert(key).inserted, keys.count <= 256, take(58), value(depth: depth + 1) else { return false }
                    if take(125) { return true }
                    guard take(44) else { return false }
                    if take(125) { return true }
                }
                return false
            }
            if take(91) {
                if take(93) { return true }
                while index < tokens.count {
                    guard value(depth: depth + 1) else { return false }
                    if take(93) { return true }
                    guard take(44) else { return false }
                    if take(93) { return true }
                }
                return false
            }
            if case .string = tokens[index] {
                index += 1
                // The fixed page uses a fallback between string language literals.
                while take(124) {
                    guard take(124), index < tokens.count, case .string = tokens[index] else { return false }
                    index += 1
                }
                return true
            }
            if ["true", "false", "null"].contains(where: { tokens[index] == .word($0) }) { index += 1; return true }
            _ = take(45)
            guard index < tokens.count, case .word(let text, escaped: false) = tokens[index],
                  text.range(of: #"^(?:0|[1-9][0-9]*)(?:\.[0-9]+)?$"#, options: .regularExpression) != nil else { return false }
            index += 1
            return true
        }
    }

    private static func tokenize(_ script: String) -> [Token]? {
        guard script.utf8.count <= 65_536 else { return nil }
        let bytes = Array(script.utf8)
        var result: [Token] = []
        var index = 0
        func wordByte(_ byte: UInt8) -> Bool {
            (48...57).contains(byte) || (65...90).contains(byte) || (97...122).contains(byte) || byte == 95 || byte == 36 || byte >= 128
        }
        func hex(_ byte: UInt8) -> UInt32? {
            switch byte {
            case 48...57: return UInt32(byte - 48)
            case 65...70: return UInt32(byte - 65 + 10)
            case 97...102: return UInt32(byte - 97 + 10)
            default: return nil
            }
        }
        // Called immediately after the `u`; only identifier decoding uses the
        // result. Strings retain their raw bytes and an escaped marker.
        func unicodeEscape(_ cursor: inout Int) -> UInt32? {
            let braced = cursor < bytes.count && bytes[cursor] == 123
            if braced { cursor += 1 }
            var code: UInt32 = 0
            var digits = 0
            while cursor < bytes.count, braced ? bytes[cursor] != 125 : digits < 4 {
                guard let digit = hex(bytes[cursor]), digits < 6 else { return nil }
                code = code * 16 + digit
                digits += 1; cursor += 1
            }
            guard braced ? digits > 0 && cursor < bytes.count && bytes[cursor] == 125 : digits == 4,
                  code <= 0x10ffff else { return nil }
            if braced { cursor += 1 }
            return code
        }
        while index < bytes.count {
            let byte = bytes[index]
            if [9, 10, 12, 13, 32].contains(byte) { index += 1; continue }
            if byte == 47, index + 1 < bytes.count {
                if bytes[index + 1] == 47 {
                    index += 2
                    while index < bytes.count, bytes[index] != 10, bytes[index] != 13 { index += 1 }
                    continue
                }
                if bytes[index + 1] == 42 {
                    index += 2
                    while index + 1 < bytes.count, !(bytes[index] == 42 && bytes[index + 1] == 47) { index += 1 }
                    guard index + 1 < bytes.count else { return nil }
                    index += 2; continue
                }
            }
            if byte == 34 || byte == 39 {
                index += 1
                let start = index
                var escaped = false
                while index < bytes.count, bytes[index] != byte {
                    guard bytes[index] >= 32 else { return nil }
                    if bytes[index] == 92 {
                        escaped = true; index += 1
                        guard index < bytes.count, bytes[index] >= 32 else { return nil }
                        let escape = bytes[index]
                        index += 1
                        if escape == 117 {
                            guard unicodeEscape(&index) != nil else { return nil }
                        } else if escape == 120 {
                            guard index + 1 < bytes.count, hex(bytes[index]) != nil, hex(bytes[index + 1]) != nil else { return nil }
                            index += 2
                        } else if escape == 48, index < bytes.count, (48...57).contains(bytes[index]) { return nil }
                    } else { index += 1 }
                }
                guard index < bytes.count, let text = String(bytes: bytes[start..<index], encoding: .utf8) else { return nil }
                result.append(.string(text, escaped: escaped)); index += 1
            } else if (48...57).contains(byte) {
                // Keep decimal bytes together so `1 . 2` is not silently repaired
                // into a valid number. Other numeric/expression syntax stays unsupported.
                let start = index
                while index < bytes.count, (48...57).contains(bytes[index]) { index += 1 }
                if index + 1 < bytes.count, bytes[index] == 46, (48...57).contains(bytes[index + 1]) {
                    index += 1
                    while index < bytes.count, (48...57).contains(bytes[index]) { index += 1 }
                }
                result.append(.word(String(decoding: bytes[start..<index], as: UTF8.self)))
            } else if wordByte(byte) || byte == 92 {
                var word: [UInt8] = []
                var escaped = false
                while index < bytes.count {
                    if wordByte(bytes[index]) { word.append(bytes[index]); index += 1; continue }
                    guard bytes[index] == 92 else { break }
                    escaped = true; index += 1
                    guard index < bytes.count, bytes[index] == 117 else { return nil }
                    index += 1
                    guard let code = unicodeEscape(&index), let scalar = UnicodeScalar(code) else { return nil }
                    if code < 128 {
                        guard wordByte(UInt8(code)) else { return nil }
                    }
                    word.append(contentsOf: String(scalar).utf8)
                }
                result.append(.word(String(decoding: word, as: UTF8.self), escaped: escaped))
            } else {
                // Template literals need expression evaluation; they are not a
                // supported declaration format. Fail closed, including malformed strings.
                guard byte != 96 else { return nil }
                result.append(.punctuation(byte)); index += 1
            }
            guard result.count <= 16_384 else { return nil }
        }
        return result
    }
}
