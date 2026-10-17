import Foundation

@main struct EventLogFileTests {
    static func main() throws {
        var passed = 0
        func check(_ value: Bool, _ name: String) {
            guard value else { fatalError("FAIL: \(name)") }
            passed += 1; print("PASS: \(name)")
        }
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("szu-fixture-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let url = folder.appendingPathComponent("events.json")
        let events = (0..<301).map { ConnectionEvent(date: Date(), message: "fixture \($0)") }
        try JSONEncoder().encode(events).write(to: url)
        let loaded = try EventLogFile.read(from: url)
        check(loaded.count == 300 && loaded[0].id == events[0].id, "Existing event schema and newest entries stay compatible")
        let oversized = Data(repeating: 32, count: EventLogFile.maximumBytes + 8)
        try oversized.write(to: url)
        do { _ = try EventLogFile.read(from: url); fatalError("Oversized log accepted") }
        catch { check((error as NSError).code == CocoaError.fileReadTooLarge.rawValue, "Oversized input is rejected by a bounded read") }
        check((try Data(contentsOf: url)) == oversized, "Failed load never rewrites original bytes")
        let first = try EventLogFile.quarantine(url, date: Date(timeIntervalSince1970: 100))
        check(!FileManager.default.fileExists(atPath: url.path), "Recovery moves the unreadable original out of the active log path")
        check((try Data(contentsOf: first)) == oversized, "Recovery backup retains every original byte")
        try Data("broken".utf8).write(to: url)
        let second = try EventLogFile.quarantine(url, date: Date(timeIntervalSince1970: 100))
        check(first != second && FileManager.default.fileExists(atPath: first.path), "Same-second recoveries never collide or replace prior backup")
        for malformed in [Data("broken".utf8), try JSONEncoder().encode([ConnectionEvent(date: Date(), message: String(repeating: "a", count: 8193))]),
                          try JSONEncoder().encode([ConnectionEvent(date: Date(timeIntervalSinceReferenceDate: 1e100), message: "fixture")])] {
            try malformed.write(to: url)
            do { _ = try EventLogFile.read(from: url); fatalError("Malformed log accepted") }
            catch { check(true, "Damaged payload is rejected before rendering") }
            check((try Data(contentsOf: url)) == malformed, "Rejected event data remains available for quarantine")
        }
        check(RecordMessageClassifier.isFailure("fixture-old-failure with details", legacyMarkers: ["fixture-old-failure"]),
              "Older consumer failure markers remain recognized without rewriting stored messages")
        for key in ["portalError.rejected", "portalError.tls", "portalError.timeout", "event.offlineAfterOnline"] {
            check(RecordMessageClassifier.isFailure(SharedCopy.t(key), legacyMarkers: []),
                  "The current shared failure text appears in the failures-only list")
        }
        check(!RecordMessageClassifier.isFailure(SharedCopy.t("portalError.authenticationUncertain"), legacyMarkers: []),
              "An unknown submission result is not mislabeled as a confirmed failure")
        check(!RecordMessageClassifier.isFailure(SharedCopy.t("event.online"), legacyMarkers: []),
              "A successful detection is not selected by the failures-only filter")
        print("\(passed) disposable-file event-store assertions passed. No formal user data or network was accessed.")
    }
}
