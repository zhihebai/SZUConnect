import Foundation
import CoreFoundation

/// Authentication targets are deliberately fixed to the upstream campus endpoints.
enum CampusArea: String, CaseIterable, Identifiable, Codable, Sendable {
    case automatic, teaching, dormitory
    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: "自动选择"
        case .teaching: "教学 / 办公区"
        case .dormitory: "宿舍区"
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
        case .automatic: "先检查校园网登录服务，再选择教学或宿舍区。发送账号密码后，不会换区重试。"
        case .teaching: "通过教学 / 办公区登录，使用 HTTPS 加密。"
        case .dormitory: "通过宿舍区登录。账号密码会以明文 HTTP 发送，自动重连前请先信任当前网络。"
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
        case .invalidCredentials: "请填写校园网账号和密码。"
        case .network: "暂时连不上校园网登录服务。请检查网络，以及 VPN、TUN 或代理工具中的校园地址规则。"
        case .timeout: "校园登录服务响应超时，可能与服务器、VPN 或分流规则有关。请先检测连接并查看登录状态。"
        case .dns: "找不到校园网登录地址。请检查网络、DNS 和代理解析规则，不能据此判断账号或密码有误。"
        case .redirect: "校园网登录服务要求跳转，没有继续访问。请核对官方登录页及 VPN、代理规则，不要向未知页面填写账号。"
        case .httpStatus: "校园网登录服务暂时无法处理请求，请稍后重试。"
        case .responseTooLarge: "登录服务返回的数据过大，已停止处理。"
        case .authenticationUncertain: "登录请求已发送，结果还不确定。请先检测连接，再决定是否重试。"
        case .protocolConflict: "登录页面和服务器返回的信息不一致，已停止登录。请重新检测，或打开校园网官方登录页。"
        case .tls: "无法确认登录服务的安全连接，已停止登录。请检查时间、证书和代理的 HTTPS 解密设置，不要跳过证书检查。"
        case .response: "无法识别校园网登录服务返回的结果。"
        case .rejected: "登录未通过，请检查账号、密码和所选区域。"
        case .cryptography: "无法准备登录所需的数据，请重试。"
        case .forbiddenTarget: "已阻止向未知服务器发送请求。"
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
    var title: String { self == .quick ? "快速检测" : "完整检测" }
    var detail: String {
        self == .quick ? "任一基础项通过，或发现明确校园登录跳转时结束，不检查常用网址；跳转不代表已联网。"
            : "检查 Apple、Microsoft 和本次勾选的网址，等待各项结果。"
    }
}

enum ConnectivityEndpointState: String, Sendable {
    case success, timeout, dns, tls, redirect, unexpectedResponse, network, cancelled, notCompleted

    var title: String {
        switch self {
        case .success: "通过"
        case .timeout: "超时"
        case .dns: "找不到地址"
        case .tls: "安全连接失败"
        case .redirect: "要求跳转"
        case .unexpectedResponse: "返回内容不符"
        case .network: "暂时连不上"
        case .cancelled: "已取消"
        case .notCompleted: "另一项已通过，停止等待"
        }
    }
}

struct ConnectivityEndpointResult: Sendable {
    let state: ConnectivityEndpointState
    let elapsed: TimeInterval
    /// A fixed portal homepage was requested by a redirect; the target is never retained.
    let campusRedirect: Bool
    init(state: ConnectivityEndpointState, elapsed: TimeInterval, campusRedirect: Bool = false) {
        self.state = state
        self.elapsed = elapsed
        self.campusRedirect = state == .redirect && campusRedirect
    }
    var succeeded: Bool { state == .success }
    var summary: String {
        succeeded ? "通过 \(Int((elapsed * 1000).rounded())) ms" : state.title
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
            return allPassed ? "完整检测全部通过" : (online ? "完整检测部分通过" : "完整检测均未通过")
        }
        return online ? "快速检测通过" : "快速检测未通过"
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
        case .online: "基础联网检测通过"
        case .portal: "检测遇到跳转"
        case .response: "检测返回内容异常"
        case .security: "无法确认安全连接"
        case .dns: "找不到网站地址"
        case .timeout: "连接超时"
        case .unreachable: "暂时连不上外网"
        case .incomplete: "检测未完成"
        }
    }

    var explanation: String {
        switch self {
        case .online: "至少一个检测地址可以访问，其他网站和校园网登录仍需分别确认。"
        case .portal: "检测遇到跳转，可能来自校园登录页、网站或代理。请核对实际页面，不能仅凭跳转确认尚未登录。"
        case .response: "检测地址返回了非预期内容，可能与登录页面、VPN、代理规则或网站服务有关，不能据此确认校园网掉线。"
        case .security: "无法确认网站证书是否可信。请检查设备时间、证书及代理的 HTTPS 解密设置；不要跳过证书检查。"
        case .dns: "找不到检测网站的地址。请检查 DNS、VPN 和代理解析规则；域名解析失败不能单独证明校园网未登录。"
        case .timeout: "检测地址未及时响应，可能与网络、VPN 或代理节点、分流规则及服务异常有关，不能单独判断校园认证状态。"
        case .unreachable: "检测地址未连接成功。请检查 Wi-Fi 或网线、VPN、代理和 DNS；仅凭本次结果无法确认校园网是否已登录。"
        case .incomplete: "检测已取消，还没有结果。"
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

    /// Read only the fixed HTTPS page. A missing, invalid or conflicting controller must
    /// stop before credentials are submitted to an assumed controller.
    static func acID(in data: Data) throws -> String {
        guard data.count <= 1_048_576 else { throw PortalError.responseTooLarge }
        guard let html = String(data: data, encoding: .utf8) else { throw PortalError.response }
        func values(_ pattern: String, in text: String) throws -> [String] {
            let regex = try NSRegularExpression(pattern: pattern, options: .caseInsensitive)
            return regex.matches(in: text, range: NSRange(text.startIndex..., in: text)).flatMap { match in
                (1..<match.numberOfRanges).compactMap { index in
                    Range(match.range(at: index), in: text).map { String(text[$0]) }
                }
            }
        }
        // The live fixed HTTPS page uses `acid`; legacy pages use `ac_id`. Treat both as
        // one controller and reject invalid or conflicting values before any submission.
        var candidates = try values(#"(?<![A-Za-z0-9_-])(?:ac_id|acid|"(?:ac_id|acid)"|'(?:ac_id|acid)')\s*[:=]\s*(?:"([^"]*)"|'([^']*)'|([^\s&"'<>;,)}\]]*))"#, in: html)
        let inputs = try NSRegularExpression(pattern: #"<input\b[^>]*>"#, options: .caseInsensitive)
        for match in inputs.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
            guard let range = Range(match.range, in: html) else { continue }
            let tag = String(html[range])
            let names = try values(#"(?<![A-Za-z0-9_:-])name\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))"#, in: tag)
            guard names.contains(where: { ["ac_id", "acid"].contains($0) }) else { continue }
            let ids = try values(#"(?<![A-Za-z0-9_:-])value\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))"#, in: tag)
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

    static func shouldFallback(after error: Error) -> Bool {
        guard let error = error as? PortalError else { return false }
        switch error {
        case .network, .response, .timeout, .dns, .httpStatus: return true
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
    func cancel() async
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
        var campusRedirect = false
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
        } catch ProbeRedirect.campus {
            state = .redirect
            campusRedirect = true
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
        return ConnectivityEndpointResult(state: state, elapsed: max(0, elapsed), campusRedirect: campusRedirect)
    }

    /// Read-only, credential-free and bounded; identity is never cached across paths.
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        let generation = transportGeneration
        let result = await confirmedCampusArea(area: area, generation: generation)
        return (try? checkGeneration(generation)) != nil ? result : nil
    }

    private func confirmedCampusArea(area: CampusArea, generation: UInt64) async -> CampusArea? {
        guard !Task.isCancelled, generation == transportGeneration else { return nil }
        do {
            // Fixed read-only page observed to return 200. Never follow homepage redirects.
            let (data, response) = try await idempotentGet(base: "https://net.szu.edu.cn", path: "/srun_portal_pc", purpose: .discovery, generation: generation)
            guard let url = response.url,
                  PortalWire.isCampusHomepage(data, status: response.statusCode, url: url)
            else { return nil }
            try Task.checkCancellation()
            if area == .teaching { return .teaching }
            do {
                let (dormData, dormResponse) = try await idempotentGet(base: "http://172.30.255.42:801", path: "/eportal", purpose: .discovery, generation: generation)
                if let dormURL = dormResponse.url,
                   PortalWire.isDormitoryHomepage(dormData, status: dormResponse.statusCode,
                                                 url: dormURL, campusConfirmed: true) { return .dormitory }
            } catch {
                if Task.isCancelled || generation != transportGeneration || error is CancellationError || (error as? PortalError) == .tls { return nil }
            }
            return area == .automatic ? .teaching : nil
        } catch {
            // Any HTTPS identity failure, including TLS, stops before plaintext discovery.
            return nil
        }
    }

    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        guard !credentials.account.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              !credentials.password.isEmpty, credentials.account.utf8.count <= 256,
              credentials.password.utf8.count <= 1024 else { throw PortalError.invalidCredentials }
        try Task.checkCancellation()
        let generation = transportGeneration
        let message: String
        switch area {
        case .teaching: message = try await teaching(credentials, generation: generation)
        case .dormitory: message = try await dormitory(credentials, generation: generation)
        case .automatic:
            do { message = try await teaching(credentials, generation: generation) }
            catch is CancellationError { throw CancellationError() }
            catch {
                try Task.checkCancellation()
                guard PortalWire.shouldFallback(after: error),
                      await confirmedCampusArea(area: .dormitory, generation: generation) == .dormitory else { throw error }
                try Task.checkCancellation()
                message = try await dormitory(credentials, generation: generation)
            }
        }
        try checkGeneration(generation)
        return message
    }

    private func teaching(_ credentials: Credentials, generation: UInt64) async throws -> String {
        let base = "https://net.szu.edu.cn"
        // A controller must come from this attempt's successfully read HTTPS page.
        let (homepage, _) = try await idempotentGet(base: base, path: "/srun_portal_pc", purpose: .authenticationDiscovery, generation: generation)
        let acID = try PortalWire.acID(in: homepage)
        try Task.checkCancellation()
        // The challenge carries no password and authenticates nothing, so it may be retried once.
        let (challengeData, _) = try await idempotentGet(base: base, path: "/cgi-bin/get_challenge", parameters: [
            URLQueryItem(name: "callback", value: "_"), URLQueryItem(name: "username", value: credentials.account),
            URLQueryItem(name: "ip", value: "")
        ], purpose: .authentication, generation: generation)
        let challengeObject: [String: Any]
        do { challengeObject = try PortalWire.object(from: challengeData) }
        catch { throw PortalError.protocolConflict }
        let (challenge, ip) = try PortalWire.challenge(from: challengeObject)
        let payload = try PortalWire.payload(credentials: credentials, challenge: challenge, ip: ip, acID: acID)
        #if os(macOS)
        let os = "macOS"
        #else
        let os = "iOS"
        #endif
        let values = ["callback": "_", "action": "login", "username": credentials.account,
                      "password": "{MD5}" + payload.hash, "os": os, "name": os, "double_stack": "0",
                      "info": payload.info, "chksum": payload.checksum, "ac_id": acID,
                      "ip": ip, "n": "200", "type": "1"]
        return try await authenticationSubmission {
            let (data, _) = try await self.get(base: base, path: "/cgi-bin/srun_portal",
                parameters: values.sorted { $0.key < $1.key }.map { URLQueryItem(name: $0.key, value: $0.value) }, generation: generation)
            switch PortalWire.teachingVerdict(try PortalWire.object(from: data)) {
            case .accepted: break
            case .rejected: throw PortalError.rejected
            case .uncertain: throw PortalError.authenticationUncertain
            }
            return "教学 / 办公区登录已通过"
        }
    }

    private func dormitory(_ credentials: Credentials, generation: UInt64) async throws -> String {
        try await authenticationSubmission {
            let (data, _) = try await self.get(base: "http://172.30.255.42:801", path: "/eportal/portal/login", parameters: [
                URLQueryItem(name: "login_method", value: "1"),
                URLQueryItem(name: "user_account", value: ",0," + credentials.account),
                URLQueryItem(name: "user_password", value: credentials.password)
            ], generation: generation)
            switch PortalWire.dormitoryVerdict(try PortalWire.object(from: data)) {
            case .accepted: break
            case .rejected: throw PortalError.rejected
            case .uncertain: throw PortalError.authenticationUncertain
            }
            return "宿舍区登录已通过"
        }
    }

    /// Once a credential-bearing submission starts, an availability/parse failure
    /// cannot establish that the server did not authenticate. Never resubmit elsewhere.
    private func authenticationSubmission(_ operation: () async throws -> String) async throws -> String {
        try Task.checkCancellation()
        do { return try await operation() }
        catch is CancellationError { throw CancellationError() }
        catch let error as PortalError {
            switch error {
            case .network, .timeout, .dns, .httpStatus, .response, .responseTooLarge, .redirect:
                throw PortalError.authenticationUncertain
            default: throw error
            }
        }
    }

    private enum RequestPurpose { case authentication, authenticationDiscovery, discovery, probe }
    private enum ProbeRedirect: Error { case campus }

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
            if isProbe, CampusRedirectTarget.matches(response.value(forHTTPHeaderField: "Location"), relativeTo: url) {
                throw ProbeRedirect.campus
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
    private func idempotentGet(base: String, path: String, parameters: [URLQueryItem] = [],
                               purpose: RequestPurpose, generation: UInt64) async throws -> (Data, HTTPURLResponse) {
        do {
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
                        try await physical.data(for: request, timeout: timeout, limit: limit)
                    }
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
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        let generation = transportGeneration
        var result = CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
        guard !Task.isCancelled else { return result }
        do {
            let (data, response) = try await idempotentGet(base: "https://net.szu.edu.cn", path: "/srun_portal_pc", purpose: .discovery, generation: generation)
            guard let url = response.url, PortalWire.isCampusHomepage(data, status: response.statusCode, url: url) else { return result }
        } catch {
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
            }
        } catch {
            if error is CancellationError {
                return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
            }
            result.dormitory = .unconfirmed
        }
        guard (try? checkGeneration(generation)) != nil else { return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked) }
        return result
    }

    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        guard !credentials.account.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              !credentials.password.isEmpty, credentials.account.utf8.count <= 256,
              credentials.password.utf8.count <= 1024 else { return LoginAttempt(error: PortalError.invalidCredentials) }
        if Task.isCancelled { return .cancelled }
        let generation = transportGeneration
        let result = area == .dormitory ? await dormitoryAttempt(credentials, generation: generation)
            : await teachingAttempt(credentials, generation: generation)
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

    private func teachingAttempt(_ credentials: Credentials, generation: UInt64) async -> LoginAttempt {
        let base = "https://net.szu.edu.cn"
        let homepage: Data
        do {
            homepage = try await idempotentGet(base: base, path: "/srun_portal_pc", purpose: .authenticationDiscovery, generation: generation).0
        } catch {
            if Task.isCancelled || error is CancellationError { return .cancelled }
            return Self.diagnosed(LoginAttempt(error: error), stage: .homepage, path: .homepage,
                                  code: Self.diagnosticCode(for: error))
        }
        let acID: String
        do { acID = try PortalWire.acID(in: homepage) }
        catch { return Self.diagnosed(LoginAttempt(error: error), stage: .homepage, path: .homepage,
                                      code: Self.diagnosticCode(for: error)) }
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
        } catch let error as PortalError where error == .rejected {
            // No challenge was issued and nothing was submitted: not a verdict about the account.
            return Self.diagnosed(LoginAttempt(outcome: .challengeRefused, error: .rejected,
                                               message: "登录服务未允许继续，未提交密码，也未发起正式登录。"),
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
                attempt = .success("教学 / 办公区登录已通过"); code = .accepted
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
                attempt = .success("宿舍区登录已通过"); code = .accepted
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
