import Foundation

private func core(_ apple: ConnectivityEndpointState, _ microsoft: ConnectivityEndpointState,
                  _ mode: ConnectivityProbeMode) -> ConnectivityReport {
    .init(appleResult: .init(state: apple, elapsed: 0.02),
          microsoftResult: .init(state: microsoft, elapsed: 0.03), mode: mode)
}

private func website(_ state: WebsiteProbeState, _ input: String,
                     _ strategy: WebsiteProbeStrategy) -> WebsiteProbeResult {
    .init(state: state, title: "Fixture", detail: "In memory only", statusCode: state == .accessible ? 200 : nil,
          elapsed: 0.01, displayURL: input, strategy: strategy)
}

private actor FakeWebsites: WebsiteChecking {
    let states: [WebsiteProbeState]
    private var calls: [(String, WebsiteProbeStrategy)] = []
    init(_ states: [WebsiteProbeState] = [.accessible]) { self.states = states }
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        let index = calls.count
        calls.append((input, strategy))
        return website(states[min(index, states.count - 1)], input, strategy)
    }
    func snapshot() -> [(String, WebsiteProbeStrategy)] { calls }
}

/// Each participant must start before any is allowed to finish. This would hang
/// a serial implementation; the bounded watchdog releases the gate and fails it.
private actor StartGate {
    private var names: [String] = []
    private var waiting: [CheckedContinuation<Void, Never>] = []
    private var released = false
    func arrive(_ name: String) async {
        names.append(name)
        if released { return }
        await withCheckedContinuation { waiting.append($0) }
    }
    func count() -> Int { names.count }
    func release() {
        released = true
        let pending = waiting; waiting = []
        for continuation in pending { continuation.resume() }
    }
}

private struct GatedWebsites: WebsiteChecking {
    let gate: StartGate
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        await gate.arrive(input)
        // Intentionally ignore Task.isCancelled to verify the coordinator suppresses
        // late success from a third-party checker after the user has cancelled.
        return website(.accessible, input, strategy)
    }
}

@main
struct InternetCheckTests {
    static func main() async throws {
        setbuf(stdout, nil)
        var passed = 0
        func check(_ value: @autoclosure () -> Bool, _ label: String) {
            guard value() else { fatalError("FAIL: \(label)") }
            passed += 1; print("PASS: \(label)")
        }
        func rejects(_ targets: [WebsiteTarget]) -> Bool {
            do { _ = try WebsiteTarget.validated(targets); return false } catch { return true }
        }
        let one = WebsiteTarget(id: "one", url: "one.invalid")
        let two = WebsiteTarget(id: "two", url: "https://two.invalid/path")
        let three = WebsiteTarget(id: "three", url: "https://three.invalid")

        check(one.enabled && one.id == "one", "Targets preserve explicit identity and default enabled state")
        check(one.displayName == "one.invalid", "Display name contains only normalized host")
        check(WebsiteTarget(url: "https://example.invalid/private/path").displayName == "example.invalid", "Display name excludes URL path")
        let validated = try WebsiteTarget.validated([.init(id: "one", url: " HTTPS://ONE.INVALID:443 "), two])
        check(validated[0].url == "https://one.invalid/", "Canonical input strips default port and normalizes root path")
        check(validated[0].id == "one" && validated[1].url == "https://two.invalid/path", "Validation preserves identity and non-root path")
        check(rejects([one, .init(url: "https://ONE.invalid/")]), "Canonical duplicates are rejected")
        let twelve = (1...12).map { WebsiteTarget(id: "expanded-\($0)", url: "https://expanded-\($0).invalid/") }
        check(!rejects(twelve), "Twelve unique configured targets are accepted")
        check(rejects(twelve + [.init(id: "thirteen", url: "https://thirteen.invalid/")]), "A thirteenth configured target is rejected")
        check(rejects([one, .init(id: "one", url: "unique.invalid")]), "Duplicate identities are rejected")
        check(rejects([.init(id: "", url: "one.invalid")]), "Empty target identity is rejected")
        check(rejects([.init(id: String(repeating: "a", count: 129), url: "one.invalid")]), "Overly long target identity is rejected")
        check(rejects([.init(id: "bad\nidentity", url: "one.invalid")]), "Control characters in target identity are rejected")
        check(rejects([.init(url: "https://one.invalid/" + String(repeating: "汉", count: 400))]), "Post-normalization percent-encoded address length is capped")
        for value in ["", "file:///private", "https://user:secret@one.invalid", "https://one.invalid?token=private", "https://one.invalid/#private"] {
            check(rejects([.init(url: value)]), "Malformed or sensitive target is rejected")
        }
        let empty = try WebsiteTarget.validated([])
        check(empty.isEmpty, "An empty target list is valid")

        let allFake = FakeWebsites()
        let all = await InternetChecker.check(mode: .complete, targets: [one, two, three], checker: allFake) { mode in
            core(.success, .success, mode)
        }
        check(all.verdict == .allPassed && !all.hasOnlyWebsiteIssues, "Both core probes and all selected websites pass")
        check(all.selectedCount == 3 && all.includedCount == 3 && all.websitePassedCount == 3, "Report aggregates configured and actual counts")
        check(all.websites.map(\.targetID) == ["one", "two", "three"], "Concurrent website results retain configured order")
        let allCalls = await allFake.snapshot()
        check(allCalls.count == 3 && allCalls.allSatisfy { $0.1 == .system }, "Integrated probes always follow system proxy strategy")
        check(all.base.online && all.base.allPassed, "Aggregate report preserves unmodified core connectivity")

        for state: WebsiteProbeState in [.redirected, .httpStatus, .authenticationRequired, .dns, .tls, .timeout, .network, .securityPolicy, .invalidInput, .cancelled] {
            let report = await InternetChecker.check(mode: .complete, targets: [one], checker: FakeWebsites([state])) { mode in
                core(.success, .success, mode)
            }
            check(report.verdict == .partial && report.websitePassedCount == 0, "Only HTTP accessible website state counts as a website pass")
            check(report.base.online && report.base.allPassed, "Website failure never converts core online to offline")
            check(report.hasOnlyWebsiteIssues, "A failed optional website is isolated from the passing base endpoints")
        }
        let fixtureURL = URL(string: "https://availability.fixture.invalid/")!
        func report(_ result: WebsiteProbeResult, selectedCount: Int = 1) -> InternetCheckReport {
            .init(base: core(.success, .success, .complete),
                  websites: [.init(targetID: "fixture", displayName: "fixture.invalid", result: result)], selectedCount: selectedCount)
        }
        let timeout = report(.failure(.timedOut, url: fixtureURL, strategy: .system, elapsed: 0.01))
        check(timeout.hasOnlyWebsiteIssues && timeout.hasRecoveryEvidence && !timeout.hasCampusRecoveryEvidence && !timeout.transportRecovered,
              "Proxy or website timeout does not request campus authentication when both base endpoints passed")
        for code in [200, 302, 401, 403, 404, 500] {
            let http = report(.response(code, url: fixtureURL, strategy: .system, elapsed: 0.01))
            check(!http.hasRecoveryEvidence && http.transportRecovered,
                  "Every actual non-campus HTTP response can establish transport recovery")
            check(http.verdict == (code == 200 ? .allPassed : .partial),
                  "Transport recovery does not change aggregate HTTP success semantics")
        }
        let campus = report(.response(302, url: fixtureURL, strategy: .system, elapsed: 0.01,
                                      location: "https://net.szu.edu.cn/srun_portal_pc"))
        check(campus.hasRecoveryEvidence && !campus.transportRecovered,
              "A fixed campus redirect supplies recovery evidence but cannot rearm transport")
        let missing = report(.response(200, url: fixtureURL, strategy: .system, elapsed: 0.01), selectedCount: 2)
        check(!missing.hasRecoveryEvidence && !missing.transportRecovered,
              "Missing selected results neither request recovery nor rearm its budget")
        for code: URLError.Code in [.badURL, .badServerResponse, .unsupportedURL, .secureConnectionFailed,
                                    .appTransportSecurityRequiresSecureConnection, .cancelled] {
            let failure = report(.failure(code, url: fixtureURL, strategy: .system, elapsed: 0.01))
            check(!failure.hasRecoveryEvidence && !failure.transportRecovered,
                  "Malformed, security and cancelled results supply neither recovery predicate")
        }
        let unspecified = report(.init(state: .network, title: "Fixture", detail: "Fixture", statusCode: nil,
                                       elapsed: 0.01, displayURL: nil, strategy: .system))
        check(!unspecified.hasRecoveryEvidence, "An unspecified network category defaults to no recovery signal")
        let cancelledReport = InternetCheckReport(base: timeout.base, websites: timeout.websites,
                                                  selectedCount: 1, wasCancelled: true)
        check(!cancelledReport.hasRecoveryEvidence && !cancelledReport.transportRecovered,
              "Round cancellation suppresses both availability and transport-recovery evidence")

        let customOnly = await InternetChecker.check(mode: .complete, targets: [one], checker: FakeWebsites()) { mode in
            core(.timeout, .dns, mode)
        }
        check(customOnly.verdict == .partial && !customOnly.base.online && !customOnly.hasOnlyWebsiteIssues,
              "Custom-only success is partial and cannot authorize base connectivity")
        let oneCore = await InternetChecker.check(mode: .complete, targets: [], checker: FakeWebsites()) { mode in
            core(.success, .timeout, mode)
        }
        check(oneCore.verdict == .partial && oneCore.base.online && !oneCore.hasOnlyWebsiteIssues,
              "One core endpoint passes only a partial complete check")
        let coreOnly = await InternetChecker.check(mode: .complete, targets: [], checker: FakeWebsites()) { mode in
            core(.success, .success, mode)
        }
        check(coreOnly.verdict == .allPassed, "Complete without custom targets requires both core endpoints")
        let none = await InternetChecker.check(mode: .complete, targets: [one], checker: FakeWebsites([.timeout])) { mode in
            core(.network, .timeout, mode)
        }
        check(none.verdict == .unavailable, "No successful endpoint means unavailable")

        for deferred in [false, true] {
            let checker = FakeWebsites()
            let result = await InternetChecker.check(mode: .complete, targets: [one, two], checker: checker,
                prioritizeCampusRecovery: true, deferWebsites: deferred) { mode in
                core(deferred ? .success : .timeout, deferred ? .success : .redirect, mode)
            }
            let requests = await checker.snapshot()
            check(requests.isEmpty, "Campus recovery or submission settlement never starts unrelated website requests")
            check(result.includedCount == 0 && result.selectedCount == 2 && result.websitesDeferred,
                  "Deferred websites remain explicitly untested and retain configured count")
            check(result.verdict != .allPassed && result.completeBasePassed == deferred,
                  "Full base proof is independent from all selected websites passing")
        }
        let cancelledBaseProof = InternetCheckReport(base: all.base, websites: [], selectedCount: 3, wasCancelled: true)
        check(!cancelledBaseProof.completeBasePassed && !cancelledBaseProof.hasCampusRecoveryEvidence,
              "Cancelled full base success cannot settle a submission or trigger recovery")

        let quickFake = FakeWebsites()
        let quick = await InternetChecker.check(mode: .quick, targets: [one, two], checker: quickFake) { mode in
            core(.success, .notCompleted, mode)
        }
        let quickCalls = await quickFake.snapshot()
        check(quick.verdict == .basicPassed && quick.websites.isEmpty && !quick.hasOnlyWebsiteIssues,
              "Quick check preserves early success semantics and skips websites")
        check(quick.selectedCount == 2 && quick.includedCount == 0 && quickCalls.isEmpty, "Quick mode retains configured count without sending website requests")
        check(quick.base.microsoftResult.state == .notCompleted, "Quick skipped endpoint remains stopped-waiting rather than failed")
        let quickNone = await InternetChecker.check(mode: .quick, targets: [one], checker: FakeWebsites()) { mode in
            core(.timeout, .dns, mode)
        }
        check(quickNone.verdict == .unavailable, "Quick failures remain unavailable")

        let disabledFake = FakeWebsites()
        let disabled = await InternetChecker.check(mode: .complete, targets: [one, .init(id: "off", url: "off.invalid", enabled: false)], checker: disabledFake) { mode in
            core(.success, .success, mode)
        }
        let disabledCalls = await disabledFake.snapshot()
        check(disabled.selectedCount == 1 && disabled.includedCount == 1 && disabledCalls.count == 1, "Disabled targets do not participate or enlarge selected count")

        let malformedFake = FakeWebsites()
        let malformed = await InternetChecker.check(mode: .complete, targets: twelve + [.init(url: "https://EXPANDED-1.invalid/"), .init(url: "https://private.invalid?secret=never"), .init(url: "thirteen.invalid")], checker: malformedFake) { mode in
            core(.success, .success, mode)
        }
        let malformedCalls = await malformedFake.snapshot()
        check(malformedCalls.count == 12 && malformed.includedCount == 12, "Malformed imported configuration is capped at twelve unique valid requests")
        check(malformed.selectedCount == 15 && malformed.verdict == .partial && malformed.hasOnlyWebsiteIssues,
              "Missing website results remain website-only issues when both base endpoints pass")
        check(malformedCalls.allSatisfy { !$0.0.contains("secret") }, "Sensitive configuration input never reaches injected probe")
        check(malformed.websites.map(\.targetID) == twelve.map(\.id), "Defensive filtering preserves stable unique identities")

        let gate = StartGate()
        let concurrent = Task {
            await InternetChecker.check(mode: .complete, targets: twelve, checker: GatedWebsites(gate: gate)) { mode in
                await gate.arrive("base")
                return core(.success, .success, mode)
            }
        }
        for _ in 0..<200 {
            if await gate.count() == 13 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        let started = await gate.count()
        await gate.release()
        let concurrentReport = await concurrent.value
        check(started == 13 && concurrentReport.verdict == .allPassed, "Base and all twelve website checks start concurrently")

        let cancelGate = StartGate()
        let cancelledTask = Task {
            await InternetChecker.check(mode: .complete, targets: twelve, checker: GatedWebsites(gate: cancelGate)) { mode in
                await cancelGate.arrive("base")
                return core(.success, .success, mode)
            }
        }
        for _ in 0..<200 {
            if await cancelGate.count() == 13 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        cancelledTask.cancel()
        await cancelGate.release()
        let cancelled = await cancelledTask.value
        check(cancelled.verdict == .cancelled && !cancelled.hasOnlyWebsiteIssues,
              "Parent cancellation wins over late successful responses")
        check(cancelled.websites.allSatisfy { $0.result.state == .cancelled }, "Late website successes are replaced with explicit cancellation")
        check(cancelled.websitePassedCount == 0, "Cancelled late websites do not contribute a success count")

        let precancelGate = StartGate()
        let precancelFake = FakeWebsites()
        let precancelTask = Task {
            await precancelGate.arrive("ready")
            return await InternetChecker.check(mode: .complete, targets: [one], checker: precancelFake) { mode in
                fatalError("Precancelled check must not invoke base transport")
            }
        }
        for _ in 0..<200 {
            if await precancelGate.count() == 1 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        precancelTask.cancel()
        await precancelGate.release()
        let precancelled = await precancelTask.value
        let precancelCalls = await precancelFake.snapshot()
        check(precancelled.verdict == .cancelled && precancelCalls.isEmpty, "Precancelled check invokes neither base nor website transport")
        check(precancelled.base.appleResult.state == .cancelled && precancelled.base.microsoftResult.state == .cancelled, "Precancelled base has explicit cancelled endpoint states")
        check(precancelled.selectedCount == 1, "Cancellation preserves configured count for honest UI")
        check(Set(InternetCheckVerdict.allCases.map(\.rawValue)) == ["basicPassed", "allPassed", "partial", "unavailable", "cancelled"], "Aggregate verdict keys remain stable")
        print("Internet check tests: \(passed) passed")
    }
}
