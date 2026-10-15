import Foundation

/// Website checks are credential-free, in-memory diagnostics. They never authorize
/// campus authentication and must not be added to connection logs or exports.
enum WebsiteProbeStrategy: String, CaseIterable, Sendable {
    case system, direct

    var title: String { self == .system ? "按系统设置" : "忽略系统代理" }
    var detail: String {
        self == .system
            ? "按系统代理、自动配置及例外规则连接；不保证每个网址都经过代理。VPN、TUN 或按应用分流仍可能改变路径。"
            : "本次不使用系统 HTTP／HTTPS 代理。VPN、TUN、DNS 或分流规则仍可能影响连接，不能视为绕过所有代理工具。"
    }
}

enum WebsiteAddress {
    enum ValidationError: Error, LocalizedError {
        case empty, tooLong, unsupportedScheme, sensitiveComponents, invalid
        var errorDescription: String? {
            switch self {
            case .empty: return "请填写网站地址，例如 https://www.baidu.com。"
            case .tooLong: return "地址过长，请使用网站首页地址。"
            case .unsupportedScheme: return "仅支持 http:// 或 https:// 网站地址。"
            case .sensitiveComponents: return "请去掉网址中的账号密码、问号及其后的参数、# 及其后的内容，避免泄露登录信息。"
            case .invalid: return "网址格式不正确，请检查域名或 IP 地址。"
            }
        }
    }

    /// Normalize once and reject sensitive URL components before creating a task.
    /// No input is persisted, logged, interpolated into an error or exported.
    static func normalize(_ input: String) throws -> URL {
        let trimmed = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw ValidationError.empty }
        guard trimmed.utf8.count <= 2048 else { throw ValidationError.tooLong }
        guard !trimmed.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }),
              !trimmed.contains("\\") else { throw ValidationError.invalid }
        let value: String
        if trimmed.contains("://") { value = trimmed }
        else if trimmed.hasPrefix("//") { value = "https:" + trimmed }
        else {
            // A bare host:port is allowed; a non-HTTP URI such as file:/ is not.
            if let colon = trimmed.firstIndex(of: ":"), !trimmed.hasPrefix("[") {
                let suffix = trimmed[trimmed.index(after: colon)...].split(separator: "/", maxSplits: 1).first ?? ""
                guard !suffix.isEmpty, suffix.allSatisfy(\.isNumber) else { throw ValidationError.unsupportedScheme }
            }
            value = "https://" + trimmed
        }
        guard var components = URLComponents(string: value),
              let scheme = components.scheme?.lowercased(), ["http", "https"].contains(scheme) else {
            throw ValidationError.unsupportedScheme
        }
        guard components.user == nil, components.password == nil,
              components.query == nil, components.fragment == nil else { throw ValidationError.sensitiveComponents }
        guard let host = components.host, !host.isEmpty,
              !host.unicodeScalars.contains(where: { CharacterSet.whitespacesAndNewlines.contains($0) }),
              !host.contains("%"), !host.contains("@"),
              components.port.map({ (1...65535).contains($0) }) ?? true else { throw ValidationError.invalid }
        components.scheme = scheme
        components.host = host.lowercased()
        guard let url = components.url else { throw ValidationError.invalid }
        return url
    }
}

enum WebsiteProbeState: String, Sendable {
    case accessible, redirected, httpStatus, authenticationRequired
    case dns, tls, timeout, cancelled, network, securityPolicy, invalidInput
}

/// A retry hint only. Campus identity, trust and authentication guards remain separate.
enum WebsiteRecoverySignal: Sendable {
    case none, connectionFailure, campusRedirect
}

struct WebsiteProbeResult: Sendable {
    let state: WebsiteProbeState
    let title: String
    let detail: String
    let statusCode: Int?
    let elapsed: TimeInterval
    /// Memory-only. This field is deliberately not Codable and is not logged.
    let displayURL: String?
    let strategy: WebsiteProbeStrategy
    let recoverySignal: WebsiteRecoverySignal

    init(state: WebsiteProbeState, title: String, detail: String, statusCode: Int?, elapsed: TimeInterval,
         displayURL: String?, strategy: WebsiteProbeStrategy, recoverySignal: WebsiteRecoverySignal = .none) {
        self.state = state; self.title = title; self.detail = detail; self.statusCode = statusCode
        self.elapsed = elapsed; self.displayURL = displayURL; self.strategy = strategy
        self.recoverySignal = recoverySignal
    }

    var hasHTTPResponse: Bool {
        guard (200..<600).contains(statusCode ?? 0) else { return false }
        return [.accessible, .redirected, .httpStatus, .authenticationRequired].contains(state)
    }


    static func response(_ code: Int, url: URL, strategy: WebsiteProbeStrategy, elapsed: TimeInterval,
                         location: String? = nil) -> Self {
        let state: WebsiteProbeState
        let title: String
        let detail: String
        switch code {
        case 200..<300:
            state = .accessible; title = strategy == .direct ? "绕过系统代理有响应" : "按系统设置有响应"
            detail = "本次请求收到响应（HTTP \(code)）。没有下载完整网页，尚未检查网页内容、登录功能或网速。"
        case 300..<400:
            state = .redirected; title = "网站要求跳转"
            detail = "网站要求打开另一个地址（HTTP \(code)）。本次没有继续打开，跳转后的页面还没检测。跳转也可能来自网站或代理，不能据此确认校园网需要登录。"
        case 401:
            state = .authenticationRequired; title = "网站需要登录"
            detail = "本次请求收到登录要求（HTTP 401），没有发送账号密码。该网址的登录要求与校园认证需要分别确认。"
        case 403:
            state = .httpStatus; title = "网站拒绝访问"
            detail = "本次请求被拒绝（HTTP 403），可能与网站限制或代理规则有关。不能据此判断校园网掉线。"
        default:
            state = .httpStatus; title = "网站响应异常"
            detail = "本次请求收到异常响应（HTTP \(code)）。请检查网站状态或代理规则；其他网址可能仍可正常访问。"
        }
        return Self(state: state, title: title, detail: detail, statusCode: code,
                    elapsed: elapsed, displayURL: url.absoluteString, strategy: strategy,
                    recoverySignal: (300..<400).contains(code) && CampusRedirectTarget.matches(location, relativeTo: url) ? .campusRedirect : .none)
    }

    static func failure(_ code: URLError.Code, url: URL, strategy: WebsiteProbeStrategy, elapsed: TimeInterval) -> Self {
        let state: WebsiteProbeState
        let title: String
        let detail: String
        switch code {
        case .cancelled:
            state = .cancelled; title = "检测已取消"; detail = "检测已停止，网站能否访问还不确定。"
        case .timedOut:
            state = .timeout; title = "网站响应超时"; detail = "未及时收到网站回复。VPN、代理节点、分流、DNS、隧道重连或网站服务都可能造成超时，不能据此判断校园网掉线。"
        case .cannotFindHost, .dnsLookupFailed:
            state = .dns; title = "找不到网站地址"; detail = "找不到域名对应的地址。请检查网址、DNS 和代理解析规则；浏览器可能使用不同的解析设置。此结果不能确认校园网登录状态。"
        case .secureConnectionFailed, .serverCertificateHasBadDate, .serverCertificateUntrusted,
             .serverCertificateHasUnknownRoot, .serverCertificateNotYetValid,
             .clientCertificateRejected, .clientCertificateRequired:
            state = .tls; title = "无法确认安全连接"; detail = "HTTPS 连接或证书验证失败。请检查设备时间、证书和代理的 HTTPS 解密设置。检测保留证书检查，不会发送本机证书；此结果不能确认校园网登录状态。"
        case .userAuthenticationRequired, .userCancelledAuthentication:
            state = .authenticationRequired; title = "网站或代理需要登录"; detail = "本次没有使用系统保存的账号密码，也不会弹出登录窗口。网站或代理的登录要求与校园认证需要分别确认。"
        case .appTransportSecurityRequiresSecureConnection:
            state = .securityPolicy; title = "需要 HTTPS 网址"
            detail = "系统安全策略阻止了 HTTP 连接，请改用 https:// 网址。不能据此判断网站断网；检测没有降低系统安全要求。"
        default:
            state = .network; title = "未能连接网站"; detail = "没有收到网站的有效回复。请检查网络、网址、VPN、代理节点或分流规则。该结果只针对本次网址和连接方式，不能据此判断校园网掉线。"
        }
        let recoverySignal: WebsiteRecoverySignal
        switch code {
        case .timedOut, .cannotFindHost, .dnsLookupFailed, .cannotConnectToHost,
             .networkConnectionLost, .notConnectedToInternet:
            recoverySignal = .connectionFailure
        default: recoverySignal = .none
        }
        return Self(state: state, title: title, detail: detail, statusCode: nil,
                    elapsed: elapsed, displayURL: url.absoluteString, strategy: strategy, recoverySignal: recoverySignal)
    }
}

/// Each check owns an isolated session. No cookies, cached response, credentials,
/// redirects or website contents are retained; a response header ends the request.
actor WebsiteProbe {
    private let protocolClasses: [AnyClass]?
    private let timeout: TimeInterval

    init(protocolClasses: [AnyClass]? = nil, timeout: TimeInterval = 8) {
        self.protocolClasses = protocolClasses
        self.timeout = timeout.isFinite ? max(0.05, min(timeout, 8)) : 8
    }

    func probe(_ input: String, strategy: WebsiteProbeStrategy = .system) async -> WebsiteProbeResult {
        let url: URL
        do { url = try WebsiteAddress.normalize(input) }
        catch {
            return WebsiteProbeResult(state: .invalidInput, title: "请检查网站地址",
                detail: (error as? WebsiteAddress.ValidationError)?.errorDescription ?? "网址格式不正确。",
                statusCode: nil, elapsed: 0, displayURL: nil, strategy: strategy)
        }
        let operation = WebsiteHeaderOperation(url: url, strategy: strategy, timeout: timeout)
        let configuration = Self.makeConfiguration(strategy: strategy, timeout: timeout, protocolClasses: protocolClasses)
        return await withTaskCancellationHandler {
            await operation.run(configuration: configuration)
        } onCancel: {
            operation.cancel()
        }
    }

    static func makeConfiguration(strategy: WebsiteProbeStrategy, timeout: TimeInterval, protocolClasses: [AnyClass]?) -> URLSessionConfiguration {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.urlCache = nil
        configuration.urlCredentialStorage = nil
        configuration.httpCookieStorage = nil
        configuration.httpShouldSetCookies = false
        configuration.httpCookieAcceptPolicy = .never
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = timeout
        configuration.timeoutIntervalForResource = timeout
        configuration.waitsForConnectivity = false
        if strategy == .direct { configuration.connectionProxyDictionary = [:] }
        if let protocolClasses { configuration.protocolClasses = protocolClasses }
        return configuration
    }
}

/// All mutable lifecycle state is protected by lock. This explicitly handles a
/// cancellation before the continuation/task exists and ignores late callbacks.
final class WebsiteHeaderOperation: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private let url: URL
    private let strategy: WebsiteProbeStrategy
    private let timeout: TimeInterval
    private let started = ProcessInfo.processInfo.systemUptime
    private var continuation: CheckedContinuation<WebsiteProbeResult, Never>?
    private var session: URLSession?
    private var task: URLSessionDataTask?
    private var timer: DispatchWorkItem?
    private var finished: WebsiteProbeResult?

    init(url: URL, strategy: WebsiteProbeStrategy, timeout: TimeInterval) {
        self.url = url; self.strategy = strategy; self.timeout = timeout
    }

    private var elapsed: TimeInterval { max(0, ProcessInfo.processInfo.systemUptime - started) }

    func run(configuration: URLSessionConfiguration) async -> WebsiteProbeResult {
        await withCheckedContinuation { continuation in
            lock.lock()
            if let finished { lock.unlock(); continuation.resume(returning: finished); return }
            self.continuation = continuation
            let delegateQueue = OperationQueue()
            delegateQueue.maxConcurrentOperationCount = 1
            let session = URLSession(configuration: configuration, delegate: self, delegateQueue: delegateQueue)
            var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: timeout)
            request.httpMethod = "GET"
            request.httpShouldHandleCookies = false
            request.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
            let task = session.dataTask(with: request)
            self.session = session; self.task = task
            let timer = DispatchWorkItem { [weak self] in self?.fail(.timedOut) }
            self.timer = timer
            // Start while holding lock so cancellation cannot be followed by a late resume.
            task.resume()
            DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + timeout, execute: timer)
            lock.unlock()
        }
    }

    func cancel() { fail(.cancelled) }
    private func fail(_ code: URLError.Code) {
        complete(.failure(code, url: url, strategy: strategy, elapsed: elapsed))
    }
    private func complete(_ result: WebsiteProbeResult) {
        lock.lock()
        guard finished == nil else { lock.unlock(); return }
        finished = result
        let continuation = self.continuation
        let session = self.session
        self.continuation = nil; self.session = nil; self.task = nil
        timer?.cancel(); timer = nil
        lock.unlock()
        session?.invalidateAndCancel()
        continuation?.resume(returning: result)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        // Cancel at headers before URLSession delivers or buffers the page body.
        if let response = response as? HTTPURLResponse {
            complete(.response(response.statusCode, url: url, strategy: strategy, elapsed: elapsed,
                               location: response.value(forHTTPHeaderField: "Location")))
        } else { fail(.badServerResponse) }
        completionHandler(.cancel)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        complete(.response(response.statusCode, url: url, strategy: strategy, elapsed: elapsed,
                           location: response.value(forHTTPHeaderField: "Location")))
        completionHandler(nil)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        handle(challenge: challenge, completionHandler: completionHandler)
    }

    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        handle(challenge: challenge, completionHandler: completionHandler)
    }

    private func handle(challenge: URLAuthenticationChallenge,
                        completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        if challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust {
            // Default platform TLS validation, never a trust credential or bypass.
            completionHandler(.performDefaultHandling, nil)
        } else {
            if challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodClientCertificate {
                fail(.clientCertificateRequired)
            } else {
                // A challenge never counts as success, even if a server associates
                // it with an unexpected success status in failureResponse.
                complete(WebsiteProbeResult(state: .authenticationRequired, title: "网站或代理需要登录",
                    detail: "本次没有使用系统保存的账号密码，也不会弹出登录窗口。网站或代理的登录要求与校园认证需要分别确认。",
                    statusCode: (challenge.failureResponse as? HTTPURLResponse)?.statusCode,
                    elapsed: elapsed, displayURL: url.absoluteString, strategy: strategy))
            }
            completionHandler(.cancelAuthenticationChallenge, nil)
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        // Raw error descriptions may embed URLs. Only classify their numeric code.
        fail((error as? URLError)?.code ?? .badServerResponse)
    }
}
