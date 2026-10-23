import Foundation
import Darwin

/// Explicitly saved, local-only website choices. Never included in diagnostics or connection logs.
struct WebsitePreferences: Codable {
    var schemaVersion = 1
    var targets: [Entry]
    var strategy: String?
    struct Entry: Codable {
        let id: String
        let url: String
        let enabled: Bool
    }
    init(targets: [WebsiteTarget], strategy: WebsiteProbeStrategy = .system) throws {
        self.strategy = strategy.rawValue
        self.targets = try WebsiteTarget.validated(targets).map { Entry(id: $0.id, url: $0.url, enabled: $0.enabled) }
    }
    func validatedTargets() throws -> [WebsiteTarget] {
        guard schemaVersion == 1, WebsiteProbeStrategy(rawValue: strategy ?? "system") != nil else { throw CocoaError(.coderReadCorrupt) }
        return try WebsiteTarget.validated(targets.map { WebsiteTarget(id: $0.id, url: $0.url, enabled: $0.enabled) })
    }
    static func read(_ url: URL) throws -> (targets: [WebsiteTarget], strategy: WebsiteProbeStrategy) {
        guard FileManager.default.fileExists(atPath: url.path) else { return ([], .system) }
        let handle = try FileHandle(forReadingFrom: url); defer { try? handle.close() }
        let data = try handle.read(upToCount: 32_769) ?? Data()
        guard data.count <= 32_768 else { throw CocoaError(.fileReadTooLarge) }
        let configuration = try JSONDecoder().decode(Self.self, from: data)
        return (try configuration.validatedTargets(), WebsiteProbeStrategy(rawValue: configuration.strategy ?? "system") ?? .system)
    }
    func write(_ url: URL) throws {
        _ = try validatedTargets()
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(self)
        guard data.count <= 32_768 else { throw CocoaError(.fileWriteUnknown) }
        let staged = url.deletingLastPathComponent().appendingPathComponent(".websites-\(UUID().uuidString).tmp")
        defer { try? FileManager.default.removeItem(at: staged) }
        try data.write(to: staged, options: [.atomic, .completeFileProtection])
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: staged.path)
        // Same-directory POSIX rename commits bytes and permissions together.
        guard rename(staged.path, url.path) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    }
    static func quarantine(_ url: URL) throws {
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        let backup = url.deletingLastPathComponent().appendingPathComponent("websites-unreadable-\(UUID().uuidString).json")
        try FileManager.default.copyItem(at: url, to: backup)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: backup.path)
    }
}

/// Preview transport is injected independently of campus authentication.
actor PreviewWebsiteChecker: WebsiteChecking {
    private let simulateFailure = ProcessInfo.processInfo.arguments.contains("--preview-custom-failure")
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        do { try await Task.sleep(for: .milliseconds(120)); try Task.checkCancellation() }
        catch { return WebsiteProbeResult(state: .cancelled, title: "已取消", detail: "离线演示已停止。", statusCode: nil, elapsed: 0, displayURL: nil, strategy: strategy) }
        if simulateFailure {
            return WebsiteProbeResult(state: .timeout, title: "超时（演示）", detail: "模拟网站未响应，本次未访问网络。", statusCode: nil, elapsed: 0.12, displayURL: input, strategy: strategy, recoverySignal: .connectionFailure)
        }
        return WebsiteProbeResult(state: .accessible, title: "通过（演示）", detail: "模拟 HTTP 200，本次未访问网络。",
            statusCode: 200, elapsed: 0.04, displayURL: input, strategy: strategy)
    }
}

actor UnconfiguredWebsiteChecker: WebsiteChecking {
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        WebsiteProbeResult(state: .cancelled, title: "未执行", detail: "隔离测试没有注入网站服务。", statusCode: nil, elapsed: 0, displayURL: nil, strategy: strategy)
    }
}
