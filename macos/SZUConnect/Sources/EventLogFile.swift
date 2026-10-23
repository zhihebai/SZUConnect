import Foundation

struct ConnectionEvent: Identifiable, Codable, Equatable, Sendable {
    var id = UUID()
    let date: Date
    let message: String
    let context: DiagnosticFailureContext?
    let authentication: DiagnosticAuthentication?
    var kind: String? { context == nil ? nil : "failure" }

    init(id: UUID = UUID(), date: Date, message: String, context: DiagnosticFailureContext? = nil,
         authentication: DiagnosticAuthentication? = nil) {
        self.id = id; self.date = date; self.message = message
        self.context = context.flatMap { $0.valid ? $0 : nil }
        self.authentication = authentication.flatMap { $0.valid ? $0 : nil }
    }
    private enum CodingKeys: String, CodingKey { case id, date, message, kind, context, authentication }
    init(from decoder: any Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(UUID.self, forKey: .id)
        date = try c.decode(Date.self, forKey: .date)
        message = try c.decode(String.self, forKey: .message)
        let value = try? c.decodeIfPresent(DiagnosticFailureContext.self, forKey: .context)
        context = value.flatMap { $0.valid ? $0 : nil }
        let auth = try? c.decodeIfPresent(DiagnosticAuthentication.self, forKey: .authentication)
        authentication = auth.flatMap { $0.valid ? $0 : nil }
    }
    func encode(to encoder: any Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(id, forKey: .id); try c.encode(date, forKey: .date); try c.encode(message, forKey: .message)
        if let context, context.valid {
            try c.encode("failure", forKey: .kind); try c.encode(context, forKey: .context)
        }
        if let authentication, authentication.valid { try c.encode(authentication, forKey: .authentication) }
    }
}

/// Keep legacy matching at the call site and recognize the current, reviewed failure messages.
enum RecordMessageClassifier {
    static func isFailure(_ event: ConnectionEvent, legacyMarkers: [String]) -> Bool {
        event.context != nil || isFailure(event.message, legacyMarkers: legacyMarkers)
    }
    static func isFailure(_ message: String, legacyMarkers: [String]) -> Bool {
        let uncertainMessages = [SharedCopy.t("portalError.authenticationUncertain"),
            "暂时无法确认登录结果，请先做一次完整检测。",
            "登录请求已发出，但暂未收到结果。请先做一次完整检测。",
            "登录已经提交，结果还没确认。先做一次完整检测。"]
        if uncertainMessages.contains(where: { !$0.isEmpty && message.contains($0) }) { return false }
        if legacyMarkers.contains(where: { message.contains($0) }) { return true }
        // Existing logs keep their original messages after shared copy changes.
        let previousFailures = [
            "外网检测这次没通过，稍后会再测。",
            "暂时连不上校园登录服务。确认已经连上校园网；开着代理或 VPN 的话，检查一下校园地址规则。",
            "校园登录服务没有及时回复。先检测一下连接，看看是不是已经能上网。",
            "找不到校园登录地址。检查一下网络、DNS 和代理规则。",
            "校园登录地址要求跳转，登录已停止。检查一下当前网络，或者改用学校官方登录页。",
            "校园登录服务暂时处理不了请求。",
            "校园登录服务回复异常，已经停止处理。过一会儿再试。",
            "校园登录页面的信息有误，登录已停止。重新检测一次，或者改用学校官方登录页。",
            "服务器证书没验证通过，登录已停止。检查电脑时间和代理设置。",
            "登录没有通过。核对一下账号、密码和登录区域。",
            "准备登录数据时出了问题，请重试。",
            "没能确认校园登录地址，已经停止连接。检查一下当前网络。",
            "校园服务没有允许继续登录，过一会儿再试。"
        ]
        if message.hasPrefix("登录没有完成：") || previousFailures.contains(where: { message.contains($0) }) { return true }
        let currentKeys = [
            "event.offlineAfterOnline", "portalError.network", "portalError.timeout",
            "portalError.dns", "portalError.redirect", "portalError.httpStatus",
            "portalError.responseTooLarge", "portalError.protocolConflict", "portalError.tls",
            "portalError.response", "portalError.rejected", "portalError.cryptography",
            "portalError.forbiddenTarget", "portalError.challengeRefused"
        ]
        // An uncertain submission is not a confirmed login failure.
        return currentKeys.contains { message.contains(SharedCopy.t($0)) }
    }
}


/// Bounded local storage, independent of AppModel and testable with disposable files.
enum EventLogFile {
    static let maximumBytes = 1_048_576
    static func read(from url: URL) throws -> [ConnectionEvent] {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let data = try file.read(upToCount: maximumBytes + 1) ?? Data()
        guard data.count <= maximumBytes else { throw CocoaError(.fileReadTooLarge) }
        let events = try JSONDecoder().decode([ConnectionEvent].self, from: data)
        // Reject damaged payloads as a whole, so the caller preserves their original bytes.
        guard events.allSatisfy({
            $0.date.timeIntervalSinceReferenceDate.isFinite
                && abs($0.date.timeIntervalSinceReferenceDate) < 3_155_760_000
                && $0.message.utf8.count <= 8_192
        }) else { throw CocoaError(.fileReadCorruptFile) }
        return Array(events.prefix(300))
    }
    @discardableResult static func quarantine(_ url: URL, date: Date = Date()) throws -> URL {
        let backup = url.deletingLastPathComponent().appendingPathComponent(
            "events-unreadable-\(Int(date.timeIntervalSince1970))-\(UUID().uuidString).json")
        try FileManager.default.moveItem(at: url, to: backup)
        return backup
    }
}
