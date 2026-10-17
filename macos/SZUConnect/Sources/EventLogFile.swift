import Foundation

struct ConnectionEvent: Identifiable, Codable {
    var id = UUID()
    let date: Date
    let message: String
}

/// Keep legacy matching at the call site and recognize the current, reviewed failure messages.
enum RecordMessageClassifier {
    static func isFailure(_ message: String, legacyMarkers: [String]) -> Bool {
        if legacyMarkers.contains(where: { message.contains($0) }) { return true }
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
