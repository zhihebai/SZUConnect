import Foundation

/// Website checks are credential-free, in-memory diagnostics. They never authorize
/// campus authentication and must not be added to connection logs or exports.
enum WebsiteProbeStrategy: String, CaseIterable, Sendable {
    case system, direct

    var title: String { self == .system ? SharedCopy.t("website.strategy.system.title") : SharedCopy.t("website.strategy.direct.title") }
    var detail: String {
        self == .system
            ? SharedCopy.t("website.strategy.system.detail")
            : SharedCopy.t("website.strategy.direct.detail")
    }
}

enum WebsiteAddress {
    enum ValidationError: Error, LocalizedError {
        case empty, tooLong, unsupportedScheme, sensitiveComponents, invalid
        var errorDescription: String? {
            switch self {
            case .empty: return SharedCopy.t("website.error.empty")
            case .tooLong: return SharedCopy.t("website.error.tooLong")
            case .unsupportedScheme: return SharedCopy.t("website.error.unsupportedScheme")
            case .sensitiveComponents: return SharedCopy.t("website.error.sensitiveComponents")
            case .invalid: return SharedCopy.t("website.error.invalid")
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
            state = .accessible; title = SharedCopy.t("website.state.accessible.title")
            detail = SharedCopy.f("website.state.accessible.detail", ["code": String(code)])
        case 300..<400:
            state = .redirected; title = SharedCopy.t("website.state.redirected.title")
            detail = SharedCopy.f("website.state.redirected.detail", ["code": String(code)])
        case 401:
            state = .authenticationRequired; title = SharedCopy.t("website.state.authenticationRequired.title")
            detail = SharedCopy.t("website.state.authenticationRequired.detail")
        case 403:
            state = .httpStatus; title = SharedCopy.t("website.state.forbidden.title")
            detail = SharedCopy.t("website.state.forbidden.detail")
        default:
            state = .httpStatus; title = SharedCopy.t("website.state.httpStatus.title")
            detail = SharedCopy.f("website.state.httpStatus.detail", ["code": String(code)])
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
            state = .cancelled; title = SharedCopy.t("website.state.cancelled.title"); detail = SharedCopy.t("website.state.cancelled.detail")
        case .timedOut:
            state = .timeout; title = SharedCopy.t("website.state.timeout.title"); detail = SharedCopy.t("website.state.timeout.detail")
        case .cannotFindHost, .dnsLookupFailed:
            state = .dns; title = SharedCopy.t("website.state.dns.title"); detail = SharedCopy.t("website.state.dns.detail")
        case .secureConnectionFailed, .serverCertificateHasBadDate, .serverCertificateUntrusted,
             .serverCertificateHasUnknownRoot, .serverCertificateNotYetValid,
             .clientCertificateRejected, .clientCertificateRequired:
            state = .tls; title = SharedCopy.t("website.state.tls.title"); detail = SharedCopy.t("website.state.tls.detail")
        case .userAuthenticationRequired, .userCancelledAuthentication:
            state = .authenticationRequired; title = SharedCopy.t("website.state.authenticationRequired.title"); detail = SharedCopy.t("website.state.authenticationRequired.detail")
        case .appTransportSecurityRequiresSecureConnection:
            state = .securityPolicy; title = SharedCopy.t("website.state.securityPolicy.title")
            detail = SharedCopy.t("website.state.securityPolicy.detail")
        default:
            state = .network; title = SharedCopy.t("website.state.network.title"); detail = SharedCopy.t("website.state.network.detail")
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
            return WebsiteProbeResult(state: .invalidInput, title: SharedCopy.t("website.error.title"),
                detail: (error as? WebsiteAddress.ValidationError)?.errorDescription ?? SharedCopy.t("website.error.invalid"),
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
                complete(WebsiteProbeResult(state: .authenticationRequired, title: SharedCopy.t("website.state.authenticationRequired.title"),
                    detail: SharedCopy.t("website.state.authenticationRequired.detail"),
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
