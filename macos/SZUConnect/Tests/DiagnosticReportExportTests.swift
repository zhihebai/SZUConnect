import Foundation

private actor SourceGate {
    private var entered = false
    private var released = false
    private var calls = 0
    private var enteredWaiters: [CheckedContinuation<Void, Never>] = []
    private var releaseWaiters: [CheckedContinuation<Void, Never>] = []
    func run() async {
        calls += 1; entered = true
        for waiter in enteredWaiters { waiter.resume() }; enteredWaiters.removeAll()
        if !released { await withCheckedContinuation { releaseWaiters.append($0) } }
    }
    func waitUntilEntered() async {
        if !entered { await withCheckedContinuation { enteredWaiters.append($0) } }
    }
    func release() {
        released = true
        for waiter in releaseWaiters { waiter.resume() }; releaseWaiters.removeAll()
    }
    func count() -> Int { calls }
}

private struct FakeMetadataSource: DiagnosticCrashMetadataSource {
    let gate: SourceGate
    let report: DiagnosticCrashReport
    func collect(identity: DiagnosticCrashIdentity, now: Date, cancellation: DiagnosticReportTicket) async -> DiagnosticCrashReport {
        await gate.run()
        return report
    }
}

@main
struct DiagnosticReportExportTests {
    @MainActor static func main() async throws {
        var passed = 0
        func check(_ condition: Bool, _ name: String) {
            precondition(condition, name); passed += 1; print("PASS: " + name)
        }
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent("szu-report-export-" + UUID().uuidString, isDirectory: true)
        try manager.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? manager.removeItem(at: directory) }
        let now = Date(timeIntervalSinceReferenceDate: 800_000_000)
        let identity = DiagnosticCrashIdentity(bundleIdentifier: "local.SZUConnect.test", version: "1.0.0", build: "33", executablePath: "/TestApplications/Test.app/Contents/MacOS/Test")
        func candidate(age: TimeInterval = 10, bundle: String? = nil, version: String? = nil,
                       build: String? = nil, path: String? = nil, code: String = "0x0000000B", offset: UInt64? = 16) -> DiagnosticCrashCandidate {
            DiagnosticCrashCandidate(date: now.addingTimeInterval(-age), bundleIdentifier: bundle ?? identity.bundleIdentifier,
                version: version ?? identity.version, build: build ?? identity.build, executablePath: path ?? identity.executablePath,
                exceptionCode: code, moduleOffset: offset)
        }
        let report = DiagnosticCrashReport.filtered([candidate()], identity: identity, now: now)
        check(report.status == .found && report.records.count == 1, "An exact injected app identity yields metadata")
        check(report.records.first?.moduleOffset == "0x0000000000000010", "Offsets use a bounded hexadecimal representation")
        for (name, value) in [
            ("another bundle", candidate(bundle: "local.Other.test")),
            ("another version", candidate(version: "1.0.1")),
            ("another internal build", candidate(build: "34")),
            ("same name at another path", candidate(path: "/Backup/Test.app/Contents/MacOS/Test")),
            ("seven-day-old record", candidate(age: 7 * 86_400 + 1)),
            ("future record", candidate(age: -1)),
            ("unbounded offset", candidate(offset: 0x1000_0001)),
            ("arbitrary code", candidate(code: "untrusted text")),
            ("lowercase code", candidate(code: "0x000000ab")),
            ("oversized code", candidate(code: "0x000000000000000000000000"))] {
            check(DiagnosticCrashReport.filtered([value], identity: identity, now: now).records.isEmpty, "Filtering rejects " + name)
        }
        check(DiagnosticCrashReport.filtered([candidate(age: 7 * 86_400)], identity: identity, now: now).records.count == 1, "The seven-day lower boundary is inclusive")
        check(DiagnosticCrashReport.filtered([candidate(offset: nil)], identity: identity, now: now).records.first?.moduleOffset == nil, "An unavailable offset is omitted")
        check(DiagnosticCrashReport.filtered([candidate(code: "0x000000000000000B")], identity: identity, now: now).records.count == 1, "A bounded native 64-bit code is allowed")
        let many = DiagnosticCrashReport.filtered((1...12).map { candidate(age: Double($0)) }, identity: identity, now: now)
        check(many.records.count == 5, "At most five metadata records are retained")
        check(many.records.first?.date == now.addingTimeInterval(-1) && many.records.last?.date == now.addingTimeInterval(-5), "The newest matching records are kept")
        check(DiagnosticCrashReport.filtered([], identity: identity, now: now).status == .none, "No matching records means not found, never never-crashed")
        check(DiagnosticCrashReport.filtered([], identity: identity, now: now, sourceStatus: .accessDenied).status == .accessDenied, "Denied access differs from no records")
        check(DiagnosticCrashReport.filtered([], identity: identity, now: now, sourceStatus: .unavailable).status == .unavailable, "Unavailable sources differ from no records")
        check(DiagnosticCrashReport.filtered([candidate()], identity: identity, now: now, sourceStatus: .accessDenied).status == .partial, "Available records plus denial are partial")
        check(DiagnosticCrashReport.filtered([candidate()], identity: identity, now: now, sourceStatus: .partial).status == .partial, "An incomplete source remains partial")
        check(DiagnosticCrashReport.filtered([], identity: identity, now: now, sourceStatus: .partial).status == .partial, "Incomplete sources with no matching records do not claim a complete search")
        check(DiagnosticCrashReport.filtered([candidate()], identity: identity, now: now, sourceStatus: .cancelled).status == .cancelled, "A cancelled source never claims found records")
        let invalidIdentity = DiagnosticCrashIdentity(bundleIdentifier: identity.bundleIdentifier, version: "untrusted", build: "33", executablePath: identity.executablePath)
        check(DiagnosticCrashReport.filtered([candidate()], identity: invalidIdentity, now: now).status == .unavailable, "Invalid runtime version metadata cannot qualify crash records")
        let payload = try DiagnosticReportWriter.encoded(snapshot: Data("{\"summary\":\"frozen\",\"events\":[]}".utf8), crashes: report)
        let payloadText = String(decoding: payload, as: UTF8.self)
        check(!payloadText.contains(identity.bundleIdentifier) && !payloadText.contains(identity.executablePath) && !payloadText.contains("build"), "Crash metadata never exports matching paths, identity or private source fields")
        check(payloadText.contains("0x0000000B") && payloadText.contains("frozen"), "Only allowed metadata is appended to the frozen snapshot")
        for invalid in [Data("[]".utf8), Data("not JSON".utf8), Data(repeating: 32, count: DiagnosticReportWriter.maximumSnapshotBytes + 1)] {
            do { _ = try DiagnosticReportWriter.encoded(snapshot: invalid, crashes: report); check(false, "Invalid snapshot must fail") }
            catch { check(true, "Invalid or oversized snapshots are rejected before saving") }
        }

        let destination = directory.appendingPathComponent("existing.json")
        let original = Data("original report".utf8)
        try original.write(to: destination)
        let cancelBefore = DiagnosticReportTicket()
        do {
            try DiagnosticReportWriter.save(payload, to: destination, ticket: cancelBefore, beforeCommit: { cancelBefore.cancel() })
            check(false, "Cancellation before commit must throw")
        } catch is CancellationError { check(true, "A cancellation before commit aborts final replacement") }
        check(try Data(contentsOf: destination) == original, "Cancelled replacement preserves an existing report byte for byte")
        check(try manager.contentsOfDirectory(atPath: directory.path).sorted() == ["existing.json"], "Cancellation removes the temporary file")
        let cancelAfter = DiagnosticReportTicket()
        try DiagnosticReportWriter.save(payload, to: destination, ticket: cancelAfter, afterCommit: { cancelAfter.cancel() })
        check(cancelAfter.isCommitted && !cancelAfter.isCancelled, "Cancellation after commit cannot change a successful save")
        check(try Data(contentsOf: destination) == payload, "The committed report has all new bytes")
        let slowCommit = DiagnosticReportTicket()
        let admitted = DispatchSemaphore(value: 0), releaseCommit = DispatchSemaphore(value: 0), finishedCommit = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            try! slowCommit.commit {
                admitted.signal(); releaseCommit.wait()
            }
            finishedCommit.signal()
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            DispatchQueue.global().async { admitted.wait(); continuation.resume() }
        }
        slowCommit.cancel()
        check(!slowCommit.isCancelled && !slowCommit.isCommitted, "Cancellation remains nonblocking while an admitted filesystem commit is still running")
        releaseCommit.signal()
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            DispatchQueue.global().async { finishedCommit.wait(); continuation.resume() }
        }
        check(slowCommit.isCommitted, "A commit that already began reports its eventual successful save")
        let failedDirectory = directory.appendingPathComponent("cannot-replace.json", isDirectory: true)
        try manager.createDirectory(at: failedDirectory, withIntermediateDirectories: false)
        do { try DiagnosticReportWriter.save(payload, to: failedDirectory, ticket: DiagnosticReportTicket()); check(false, "Replacing a directory must fail") }
        catch { check(true, "A commit error is reported as failure") }
        check(try manager.contentsOfDirectory(atPath: directory.path).allSatisfy { !$0.hasSuffix(".tmp") }, "Commit failure also removes temporary files")
        check(manager.fileExists(atPath: failedDirectory.path), "Failure preserves the selected existing directory")
        let alreadyCancelled = DiagnosticReportTicket(); alreadyCancelled.cancel()
        do { try DiagnosticReportWriter.save(payload, to: destination, ticket: alreadyCancelled); check(false, "Pre-cancelled task must fail") }
        catch is CancellationError { check(true, "Already-cancelled work creates no temporary file") }

        func terminal(_ coordinator: DiagnosticReportExportCoordinator) async -> DiagnosticReportExportCoordinator.State {
            if !coordinator.busy { return coordinator.state }
            for await state in coordinator.$state.values {
                if state == .saved || state == .cancelled || state == .failed { return state }
            }
            preconditionFailure("State stream ended")
        }
        let gate = SourceGate()
        let coordinator = DiagnosticReportExportCoordinator()
        coordinator.start(snapshot: Data("{}".utf8), destination: destination, identity: identity, allowCrashCollection: true,
                          source: FakeMetadataSource(gate: gate, report: report))
        check(coordinator.state == .idle, "A caller cannot begin collection without first choosing a destination")
        check(coordinator.beginChoosing(), "A fresh coordinator permits choosing a destination")
        check(!coordinator.beginChoosing(), "Repeated export is blocked during the save dialog")
        coordinator.selectionCancelled()
        check(coordinator.state == .idle && !coordinator.busy, "Cancelling the save dialog ends without collection")
        check(await gate.count() == 0, "A cancelled dialog does not invoke a metadata source")
        check(coordinator.beginChoosing(), "An export can start after dialog cancellation")
        let frozen = Data("{\"summary\":\"snapshot before changes\"}".utf8)
        coordinator.start(snapshot: frozen, destination: destination, identity: identity, allowCrashCollection: true,
                          source: FakeMetadataSource(gate: gate, report: report))
        await gate.waitUntilEntered()
        check(coordinator.state == .preparing, "Collection runs asynchronously after destination selection")
        check(!coordinator.beginChoosing(), "Repeated export is blocked during collection")
        var liveText = "snapshot before changes"; liveText = "new model value"
        check(liveText == "new model value", "The main actor can continue changing unrelated state during collection")
        coordinator.cancel()
        check(coordinator.state == .stopping, "Stopping remains busy until its worker finishes")
        check(!coordinator.beginChoosing(), "Stopping cannot overlap a new export")
        await gate.release()
        check(await terminal(coordinator) == .cancelled, "Closing or cancelling a pending export discards its result")
        check(try Data(contentsOf: destination) == payload, "Cancelled asynchronous collection preserves the preceding report")
        let successGate = SourceGate(); await successGate.release()
        check(coordinator.beginChoosing(), "A finished cancellation permits another export")
        coordinator.start(snapshot: frozen, destination: destination, identity: identity, allowCrashCollection: true,
                          source: FakeMetadataSource(gate: successGate, report: report))
        check(await terminal(coordinator) == .saved, "The asynchronous exporter commits a successful result")
        check(String(decoding: try Data(contentsOf: destination), as: UTF8.self).contains("snapshot before changes"), "Later model changes cannot enter the frozen report")
        coordinator.cancel()
        check(coordinator.state == .saved, "A close arriving after success leaves the saved outcome intact")
        let readOnlyGate = SourceGate()
        check(coordinator.beginChoosing(), "A read-only session may explicitly save its report")
        coordinator.start(snapshot: frozen, destination: destination, identity: identity, allowCrashCollection: false,
                          source: FakeMetadataSource(gate: readOnlyGate, report: report))
        check(await terminal(coordinator) == .saved, "Read-only export still produces a report")
        check(await readOnlyGate.count() == 0, "Read-only and preview reports never call the system source")
        let readOnlyText = String(decoding: try Data(contentsOf: destination), as: UTF8.self)
        check(readOnlyText.contains("readOnlyOrPreview") && readOnlyText.contains("notCollected"), "Read-only reports explicitly state metadata was not collected")
        check(coordinator.beginChoosing(), "An ordinary unsupported source may also export")
        coordinator.start(snapshot: frozen, destination: destination, identity: identity, allowCrashCollection: true)
        check(await terminal(coordinator) == .saved, "The conservative production source does not prevent saving")
        let ordinaryText = String(decoding: try Data(contentsOf: destination), as: UTF8.self)
        check(ordinaryText.contains("unsupportedMetadataSource") && !ordinaryText.contains("never"), "Ordinary reports accurately state the native collection limit")
        check(coordinator.beginChoosing(), "The save failure case begins normally")
        coordinator.start(snapshot: frozen, destination: failedDirectory, identity: identity, allowCrashCollection: false)
        check(await terminal(coordinator) == .failed, "An asynchronous save failure remains distinguishable from cancellation")
        check(try manager.contentsOfDirectory(atPath: directory.path).allSatisfy { !$0.hasSuffix(".tmp") }, "All asynchronous outcomes leave no temporary files")
        let neverStarted = SourceGate()
        check(coordinator.beginChoosing(), "An export may be cancelled immediately after choosing")
        coordinator.start(snapshot: frozen, destination: destination, identity: identity, allowCrashCollection: true,
                          source: FakeMetadataSource(gate: neverStarted, report: report))
        coordinator.cancel()
        check(await terminal(coordinator) == .cancelled, "Closing before worker startup cancels without starting collection")
        check(await neverStarted.count() == 0, "A pre-start cancellation never enters even the injected source")
        print("\(passed) diagnostic report export assertions passed")
    }
}
