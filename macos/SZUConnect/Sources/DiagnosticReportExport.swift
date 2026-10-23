import Foundation
import Combine
#if canImport(Darwin)
import Darwin
#endif

/// This identity is used only to filter an injected metadata source. Paths and
/// bundle identifiers are never encoded into a diagnostic report.
struct DiagnosticCrashIdentity: Sendable {
    let bundleIdentifier: String
    let version: String
    let build: String
    let executablePath: String
}

struct DiagnosticCrashCandidate: Sendable {
    let date: Date
    let bundleIdentifier: String
    let version: String
    let build: String
    let executablePath: String
    let exceptionCode: String
    let moduleOffset: UInt64?
}

struct DiagnosticCrashReport: Encodable, Sendable, Equatable {
    enum Status: String, Encodable, Sendable { case notCollected, found, none, accessDenied, unavailable, cancelled, partial }
    enum Reason: String, Encodable, Sendable { case unsupportedMetadataSource, readOnlyOrPreview }
    struct Record: Encodable, Sendable, Equatable {
        let date: Date
        let version: String
        let exceptionCode: String
        let moduleOffset: String?
    }
    let status: Status
    let lookbackDays: Int = 7
    let collectedAt: Date?
    let reason: Reason?
    let records: [Record]

    static func notCollected(_ reason: Reason) -> Self {
        Self(status: .notCollected, collectedAt: nil, reason: reason, records: [])
    }

    /// Used by injected sources and tests. A name alone never establishes app
    /// identity. Neither mismatched versions nor old installation paths qualify.
    static func filtered(_ candidates: [DiagnosticCrashCandidate], identity: DiagnosticCrashIdentity,
                         now: Date, sourceStatus: Status = .found) -> Self {
        if sourceStatus == .cancelled || sourceStatus == .notCollected {
            return Self(status: sourceStatus, collectedAt: nil, reason: nil, records: [])
        }
        guard now.timeIntervalSinceReferenceDate.isFinite,
              validVersion(identity.version), !identity.bundleIdentifier.isEmpty,
              !identity.build.isEmpty, identity.executablePath.hasPrefix("/") else {
            return Self(status: .unavailable, collectedAt: nil, reason: nil, records: [])
        }
        let cutoff = now.addingTimeInterval(-7 * 24 * 60 * 60)
        let matching = candidates.prefix(256).filter { candidate in
            candidate.bundleIdentifier == identity.bundleIdentifier && candidate.version == identity.version
                && candidate.build == identity.build && candidate.executablePath == identity.executablePath
                && candidate.date.timeIntervalSinceReferenceDate.isFinite && candidate.date >= cutoff && candidate.date <= now
                && candidate.exceptionCode.range(of: "^0x[0-9A-F]{8}([0-9A-F]{8})?$", options: .regularExpression) != nil
                && (candidate.moduleOffset == nil || candidate.moduleOffset! <= 0x1000_0000)
        }.sorted { $0.date > $1.date }
        let records = matching.prefix(5).map { candidate in
            Record(date: candidate.date, version: candidate.version, exceptionCode: candidate.exceptionCode,
                   moduleOffset: candidate.moduleOffset.map { String(format: "0x%016llX", $0) })
        }
        let incomplete = sourceStatus == .partial || sourceStatus == .accessDenied || sourceStatus == .unavailable || candidates.count > 256
        let emptyStatus: Status = sourceStatus == .accessDenied ? .accessDenied
            : sourceStatus == .partial || candidates.count > 256 ? .partial
            : sourceStatus == .unavailable ? .unavailable : .none
        let status: Status = records.isEmpty ? emptyStatus : incomplete ? .partial : .found
        return Self(status: status, collectedAt: now, reason: nil, records: records)
    }

    private static func validVersion(_ value: String) -> Bool {
        value.count <= 23 && value.range(of: "^[0-9]{1,5}(\\.[0-9]{1,5}){0,3}$", options: .regularExpression) != nil
    }
}

protocol DiagnosticCrashMetadataSource: Sendable {
    func collect(identity: DiagnosticCrashIdentity, now: Date, cancellation: DiagnosticReportTicket) async -> DiagnosticCrashReport
}

/// macOS does not offer a reliable, on-demand seven-day metadata API for this
/// app on all supported systems. An IPS header cannot prove the executable
/// path; reading entire crash logs or requesting extra privileges is avoided.
struct UnavailableDiagnosticCrashMetadataSource: DiagnosticCrashMetadataSource {
    func collect(identity: DiagnosticCrashIdentity, now: Date, cancellation: DiagnosticReportTicket) async -> DiagnosticCrashReport {
        .notCollected(.unsupportedMetadataSource)
    }
}

/// Cancellation and admission to the final atomic rename share one short lock.
/// The lock is never held across filesystem calls, so closing a window cannot
/// block the main actor on a slow save volume. Once commit has started, its
/// actual result decides whether the report was saved.
final class DiagnosticReportTicket: @unchecked Sendable {
    private enum Phase { case pending, cancelled, committing, committed, failed }
    private let lock = NSLock()
    private var phase = Phase.pending

    func cancel() {
        lock.lock(); defer { lock.unlock() }
        if phase == .pending { phase = .cancelled }
    }
    var isCancelled: Bool {
        lock.lock(); defer { lock.unlock() }
        return phase == .cancelled
    }
    var isCommitted: Bool {
        lock.lock(); defer { lock.unlock() }
        return phase == .committed
    }
    func commit(_ action: () throws -> Void) throws {
        lock.lock()
        guard phase == .pending else { lock.unlock(); throw CancellationError() }
        phase = .committing
        lock.unlock()
        do {
            try action()
            lock.lock(); phase = .committed; lock.unlock()
        } catch {
            lock.lock(); phase = .failed; lock.unlock()
            throw error
        }
    }
}

enum DiagnosticReportOutcome: Sendable, Equatable { case saved, cancelled, failed }

enum DiagnosticReportWriter {
    static let maximumSnapshotBytes = 8 * 1_024 * 1_024

    static func encoded(snapshot: Data, crashes: DiagnosticCrashReport) throws -> Data {
        guard snapshot.count <= maximumSnapshotBytes,
              var root = try JSONSerialization.jsonObject(with: snapshot) as? [String: Any] else {
            throw CocoaError(.fileWriteInapplicableStringEncoding)
        }
        let encoder = JSONEncoder(); encoder.dateEncodingStrategy = .iso8601
        root["crashes"] = try JSONSerialization.jsonObject(with: encoder.encode(crashes))
        return try JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys])
    }

    static func save(_ data: Data, to destination: URL, ticket: DiagnosticReportTicket,
                     beforeCommit: () -> Void = {}, afterCommit: () -> Void = {}) throws {
        guard destination.isFileURL, destination.path.hasPrefix("/"), !destination.lastPathComponent.isEmpty else {
            throw CocoaError(.fileWriteInvalidFileName)
        }
        guard !ticket.isCancelled else { throw CancellationError() }
        let temporary = destination.deletingLastPathComponent()
            .appendingPathComponent(".SZUConnect-report-" + UUID().uuidString + ".tmp")
        #if canImport(Darwin)
        let descriptor = open(temporary.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var opened = true
        defer {
            if opened { _ = close(descriptor) }
            _ = unlink(temporary.path)
        }
        try data.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                guard !ticket.isCancelled else { throw CancellationError() }
                let written = write(descriptor, raw.baseAddress!.advanced(by: offset), min(65_536, raw.count - offset))
                if written < 0 && errno == EINTR { continue }
                guard written > 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                offset += written
            }
        }
        guard fsync(descriptor) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let closed = close(descriptor); opened = false
        guard closed == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        beforeCommit()
        try ticket.commit {
            guard rename(temporary.path, destination.path) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        }
        afterCommit()
        #else
        // iOS retains the system document exporter; this writer is not invoked.
        throw CocoaError(.featureUnsupported)
        #endif
    }
}

@MainActor
final class DiagnosticReportExportCoordinator: ObservableObject {
    enum State: Equatable { case idle, choosing, preparing, stopping, saved, cancelled, failed }
    @Published private(set) var state: State = .idle
    private var ticket: DiagnosticReportTicket?
    private var task: Task<Void, Never>?
    private var worker: Task<DiagnosticReportOutcome, Never>?
    private var generation = UUID()
    var busy: Bool { state == .choosing || state == .preparing || state == .stopping }

    func beginChoosing() -> Bool {
        guard !busy else { return false }
        state = .choosing
        return true
    }
    func selectionCancelled() {
        guard state == .choosing else { return }
        state = .idle
    }

    /// Called only after a save destination has been accepted. Snapshot bytes
    /// have already been frozen synchronously on the model's main actor.
    func start(snapshot: Data, destination: URL, identity: DiagnosticCrashIdentity,
               allowCrashCollection: Bool,
               source: any DiagnosticCrashMetadataSource = UnavailableDiagnosticCrashMetadataSource()) {
        guard state == .choosing else { return }
        let current = DiagnosticReportTicket()
        let id = UUID(); generation = id; ticket = current; state = .preparing
        task = Task { [weak self] in
            let worker = Task.detached(priority: .utility) { () -> DiagnosticReportOutcome in
                guard !current.isCancelled else { return .cancelled }
                let crashes = allowCrashCollection
                    ? await source.collect(identity: identity, now: Date(), cancellation: current)
                    : DiagnosticCrashReport.notCollected(.readOnlyOrPreview)
                guard !current.isCancelled else { return .cancelled }
                do {
                    let data = try DiagnosticReportWriter.encoded(snapshot: snapshot, crashes: crashes)
                    try DiagnosticReportWriter.save(data, to: destination, ticket: current)
                    return .saved
                } catch is CancellationError { return current.isCommitted ? .saved : .cancelled }
                catch { return current.isCommitted ? .saved : current.isCancelled ? .cancelled : .failed }
            }
            self?.worker = worker
            let outcome = await worker.value
            guard let self, self.generation == id else { return }
            self.task = nil; self.worker = nil; self.ticket = nil
            self.state = outcome == .saved ? .saved : outcome == .cancelled ? .cancelled : .failed
        }
    }

    func cancel() {
        if state == .choosing { state = .cancelled; return }
        guard let ticket else { return }
        ticket.cancel()
        worker?.cancel()
        // The worker checks cancellation before and while writing. Its final
        // result owns the state, including a commit that just won the race.
        state = ticket.isCommitted ? .saved : .stopping
    }

    deinit {
        ticket?.cancel()
        worker?.cancel()
    }
}
