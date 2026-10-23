import Foundation

/// Exercises the real AppModel orchestration with scripted, entirely in-memory transport.
/// A preflight is distinct from a credential-bearing submission: unavailable/challengeRefused
/// may permit another preflight in the same round, rejected/uncertain may not.
private actor RecoveryService: ConnectionService {
    enum Stage: Hashable { case probe, discovery, preflight, attempt }
    struct Reply: Sendable {
        let result: LoginAttempt
        let submitted: Bool
    }
    struct Attempt: Sendable {
        let area: CampusArea
        let credentials: Credentials
        let submitted: Bool
    }
    struct Snapshot: Sendable {
        let probes: Int
        let discoveries: [CampusArea]
        let attempts: [Attempt]
        var submissions: [Attempt] { attempts.filter(\.submitted) }
    }
    private var reports: [ConnectivityReport] = []
    private var discoveryReplies: [CampusDiscovery] = []
    private var replies: [Reply] = []
    private var probes = 0
    private var discoveries: [CampusArea] = []
    private var attempts: [Attempt] = []
    private var paused: Set<Stage> = []
    private var waiting: [Stage: CheckedContinuation<Void, Never>] = [:]

    func configure(reports: [ConnectivityReport], discoveries: [CampusDiscovery] = [], replies: [Reply] = []) {
        self.reports = reports
        self.discoveryReplies = discoveries
        self.replies = replies
    }
    func pauseNext(_ stage: Stage) { paused.insert(stage) }
    func isWaiting(_ stage: Stage) -> Bool { waiting[stage] != nil }
    func release(_ stage: Stage) {
        guard let continuation = waiting.removeValue(forKey: stage) else { fatalError("No suspended fake call") }
        continuation.resume()
    }
    private func suspendIfRequested(_ stage: Stage) async {
        guard paused.remove(stage) != nil else { return }
        // Deliberately returns even after Task cancellation, to exercise operation ownership.
        await withCheckedContinuation { waiting[stage] = $0 }
    }
    func snapshot() -> Snapshot { Snapshot(probes: probes, discoveries: discoveries, attempts: attempts) }
    func probe() async -> ConnectivityReport {
        probes += 1
        let reply = reports.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : reports.removeFirst()
        await suspendIfRequested(.probe)
        return reply
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(area)
        guard !discoveryReplies.isEmpty else { fatalError("Unexpected discovery for \(area)") }
        let reply = discoveryReplies.removeFirst()
        await suspendIfRequested(.discovery)
        return reply
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        guard area != .automatic else { fatalError("AppModel must select an explicit area before attempting login") }
        guard !replies.isEmpty else { fatalError("Unexpected credential attempt") }
        let reply = replies.removeFirst()
        await suspendIfRequested(.preflight)
        if Task.isCancelled { return .cancelled }
        attempts.append(Attempt(area: area, credentials: credentials, submitted: reply.submitted))
        await suspendIfRequested(.attempt)
        return reply.result
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        fatalError("New AppModel integration must use staged discovery")
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        fatalError("New AppModel integration must use classified, explicit-area attempts")
    }
}

@MainActor private final class RecoveryClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct AutoRecoveryAppModelTests {
    private static var passed = 0
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let online = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0),
        microsoftResult: .init(state: .success, elapsed: 0), mode: .complete)
    private static let teaching = CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)
    private static let dormitory = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let accepted = RecoveryService.Reply(result: .success("离线认证已接受"), submitted: true)
    private static let areaRejected = RecoveryService.Reply(
        result: LoginAttempt(outcome: .rejected, rejection: .areaMismatch, error: .rejected), submitted: true)
    private static let passwordRejected = RecoveryService.Reply(
        result: LoginAttempt(outcome: .rejected, rejection: .password, error: .rejected), submitted: true)

    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        passed += 1
        print("PASS: \(description)")
    }
    private static func waitUntil(_ description: String, _ condition: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(1))
        }
        fatalError("Timed out waiting for \(description)")
    }
    private static func fixture(area: CampusArea = .automatic, trusted: Bool = true)
      -> (AppModel, RecoveryService, RecoveryClock) {
        let service = RecoveryService()
        let clock = RecoveryClock()
        let model = AppModel(client: service, isolated: true, now: { clock.date })
        model.account = "saved-fixture-account"
        model.password = "saved-fixture-password"
        model.area = area
        model.saveCredentials()
        model.receivePath(usable: true, interfaces: "offline-fixture", identity: NetworkIdentity(fingerprint: "fixture-network"))
        model.setMonitoringMode(.recover)
        if trusted { model.authorizeCurrentNetwork() }
        return (model, service, clock)
    }
    private static func finishManual(_ model: AppModel) async throws {
        try await waitUntil("manual completion") { !model.busy }
    }
    private static func checkRecover(_ model: AppModel, _ description: String) {
        check(model.monitoringMode == .recover && model.autoReconnect, description)
    }

    static func main() async throws {
        setbuf(stdout, nil)

        // Preserve actionable protocol evidence without logging the fixture's credentials.
        do {
            let (model, service, _) = fixture(area: .teaching)
            let diagnostic = AuthDiagnostic(stage: .submission, path: .teachingSubmit,
                httpStatus: 200, acID: "6", code: .explicitRejection, serverCode: .acIdError)
            let reply = RecoveryService.Reply(result: LoginAttempt(outcome: .rejected,
                rejection: .areaMismatch, error: .rejected, diagnostic: diagnostic), submitted: true)
            await service.configure(reports: [offline], discoveries: [teaching], replies: [reply])
            model.connect(); try await finishManual(model)
            let history = model.events.map(\.message).joined(separator: "\n")
            let exported = String(data: model.diagnosticExportData, encoding: .utf8) ?? ""
            check(history.contains("ac_id=6") && history.contains("server=ac_id_error")
                  && history.contains("saved=yes"), "Authentication history preserves safe controller and server evidence")
            check(exported.contains("ac_id=6") && exported.contains("server=ac_id_error"),
                  "Diagnostic export includes the latest typed authentication result")
            check(!history.contains("saved-fixture-account") && !history.contains("saved-fixture-password")
                  && !exported.contains("saved-fixture-account") && !exported.contains("saved-fixture-password"),
                  "Authentication history and export exclude account and password values")
            check(await service.snapshot().submissions.count == 1,
                  "Collecting safe diagnostics never submits a second login")
        }

        // Both entry paths discover a working dormitory before attempting any teaching login.
        for manual in [true, false] {
            let (model, service, _) = fixture()
            await service.configure(reports: manual ? [offline, online] : [offline, offline, online],
                                    discoveries: [dormitory], replies: [accepted])
            if manual { model.connect(); try await finishManual(model) }
            else {
                await model.runAutomaticCycle()
                check(await service.snapshot().attempts.isEmpty, "First background failure never authenticates")
                await model.runAutomaticCycle()
            }
            let snapshot = await service.snapshot()
            check(snapshot.discoveries == [.automatic], "Automatic choice discovers portals before \(manual ? "manual" : "background") login")
            check(snapshot.attempts.map(\.area) == [.dormitory], "Confirmed dormitory avoids the teaching candidate entirely")
            check(snapshot.submissions.count == 1, "Direct dormitory success sends one credential request")
            check(model.isOnline && model.holds.isEmpty && !model.automaticRecoveryPaused, "Dormitory success leaves no global or regional pause")
            checkRecover(model, "Dormitory success preserves the user's recover preference")
        }

        // A proven pre-submission failure may proceed to a separately confirmed dormitory.
        for outcome in [LoginOutcome.unavailable, .challengeRefused] {
            for manual in [true, false] {
                let (model, service, _) = fixture()
                let preflight = RecoveryService.Reply(result: LoginAttempt(outcome: outcome), submitted: false)
                await service.configure(reports: manual ? [offline, online] : [offline, offline, online],
                                        discoveries: [teaching, dormitory], replies: [preflight, accepted])
                if manual { model.connect(); try await finishManual(model) }
                else { await model.runAutomaticCycle(); await model.runAutomaticCycle() }
                let snapshot = await service.snapshot()
                check(snapshot.discoveries == [.automatic, .dormitory], "\(outcome): dormitory is confirmed again before fallback")
                check(snapshot.attempts.map(\.area) == [.teaching, .dormitory], "\(outcome): preflight fallback uses explicit areas")
                check(snapshot.submissions.count == 1 && snapshot.submissions.first?.area == .dormitory,
                      "\(outcome): two preflights contain exactly one credential-bearing submission")
                check(model.isOnline && model.lastAttemptOutcome == .success && model.holds.isEmpty,
                      "\(outcome): final dormitory success replaces only the preflight failure")
                checkRecover(model, "\(outcome): fallback never switches off recover")
            }
        }

        // A rejected response may follow a real submission. No same-round second submission.
        // A later round, after the cooldown, may discover dormitory and clear this candidate hold.
        do {
            let (model, service, clock) = fixture()
            await service.configure(reports: [offline], discoveries: [teaching], replies: [areaRejected])
            model.connect(); try await finishManual(model)
            let failed = await service.snapshot()
            check(failed.attempts.map(\.area) == [.teaching] && failed.submissions.count == 1,
                  "A rejected teaching submission never falls back in the same round")
            check(model.holds.count == 1 && model.holds.first?.area == .teaching
                  && model.holds.first?.origin == .candidate && model.holds.first?.sticky == false,
                  "First automatic-choice area rejection creates only a current-network teaching hold")
            check(!model.automaticRecoveryPaused, "One held candidate does not pause all automatic choices")
            checkRecover(model, "Rejected candidate preserves recover and autoReconnect")
            await service.configure(reports: [offline, offline])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().submissions.count == 1 && model.engineStatus == "coolingDown",
                  "Immediate background retries cannot bypass the 90-second submission cooldown")
            clock.advance(89)
            await model.runAutomaticCycle()
            check(await service.snapshot().submissions.count == 1, "Candidate hold does not shorten the cooldown")
            clock.advance(1)
            await service.configure(reports: [offline, online], discoveries: [dormitory], replies: [accepted])
            await model.runAutomaticCycle()
            let recovered = await service.snapshot()
            check(recovered.attempts.map(\.area) == [.teaching, .dormitory], "Later confirmed dormitory remains eligible after teaching rejection")
            check(recovered.submissions.count == 2, "Rejected and recovered rounds each submit once")
            check(model.isOnline && model.holds.isEmpty && !model.automaticRecoveryPaused,
                  "Same saved credentials succeeding in dormitory clear the prior current-network candidate hold")
            checkRecover(model, "Successful later recovery requires no toggle or manual resume")
        }

        // Public reachability is not proof that a stored password is correct.
        do {
            let (model, service, clock) = fixture(area: .teaching)
            await service.configure(reports: [offline], replies: [passwordRejected])
            model.connect(); try await finishManual(model)
            let held = model.holds
            check(held.count == 1 && held.first?.rejection == "password" && model.automaticRecoveryPaused,
                  "An explicit password rejection pauses the affected saved account and area")
            await service.configure(reports: [online])
            await model.runAutomaticCycle()
            check(model.isOnline && model.holds == held && model.automaticRecoveryPaused,
                  "A passing public probe keeps the saved-password rejection intact")
            clock.advance(91)
            await service.configure(reports: [offline, offline])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().submissions.count == 1, "Expired cooldown and repeated failures do not retry a blocked password")
            checkRecover(model, "A credential hold remains separate from the recover preference")
        }

        // Successful draft credentials are not the stored credential generation.
        do {
            let (model, service, clock) = fixture(area: .teaching)
            await service.configure(reports: [offline], replies: [passwordRejected])
            model.connect(); try await finishManual(model)
            let held = model.holds
            model.account = "unsaved-fixture-account"
            model.password = "unsaved-fixture-password"
            await service.configure(reports: [offline, online], replies: [accepted])
            model.connect(); try await finishManual(model)
            let snapshot = await service.snapshot()
            check(snapshot.submissions.count == 2 && snapshot.submissions.last?.credentials.account == "unsaved-fixture-account",
                  "Explicit draft login uses only the draft credentials")
            check(model.isOnline && model.hasUnsavedCredentialChanges && model.holds == held && model.automaticRecoveryPaused,
                  "Successful unsaved draft cannot release a saved-account rejection")
            clock.advance(91)
            await service.configure(reports: [offline, offline])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().submissions.count == 2, "Background recovery does not adopt the successful unsaved draft")
        }

        // Explicit background choices still discover that exact area and enforce trust.
        for area in [CampusArea.teaching, .dormitory] {
            let (model, service, _) = fixture(area: area, trusted: area == .teaching)
            await service.configure(reports: [offline, offline], discoveries: [area == .teaching ? teaching : dormitory],
                                    replies: area == .teaching ? [accepted] : [])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            let first = await service.snapshot()
            check(first.discoveries == [area], "Background pinned \(area) performs explicit-area discovery")
            if area == .dormitory {
                check(first.submissions.isEmpty && model.engineStatus == "needsTrust", "Untrusted dormitory discovery never sends credentials")
                model.authorizeCurrentNetwork()
                await service.configure(reports: [offline, online], discoveries: [dormitory], replies: [accepted])
                await model.runAutomaticCycle()
                check(await service.snapshot().submissions.map(\.area) == [.dormitory], "Trusted pinned dormitory submits only its selected area")
            } else {
                check(first.submissions.map(\.area) == [.teaching], "Pinned teaching never sends an automatic-area request")
            }
            checkRecover(model, "Pinned background choices preserve recover")
        }

        // If the preflight fallback would require plaintext trust, it remains blocked.
        do {
            let (model, service, _) = fixture(trusted: false)
            await service.configure(reports: [offline, offline], discoveries: [teaching, dormitory],
                                    replies: [RecoveryService.Reply(result: LoginAttempt(outcome: .challengeRefused), submitted: false)])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(snapshot.attempts.map(\.area) == [.teaching] && snapshot.submissions.isEmpty,
                  "Teaching preflight failure cannot bypass dormitory trust")
            check(model.holds.isEmpty, "Untrusted preflight failure does not poison saved credentials")
        }

        do {
            let (model, service, _) = fixture(area: .teaching)
            await service.configure(reports: [offline], replies: [passwordRejected])
            model.connect(); try await finishManual(model)
            let originalHolds = model.holds
            let object: [String: Any] = ["error": "ok", "suc_msg": "ip_already_online_error"]
            guard case .uncertain = PortalWire.teachingVerdict(object) else { fatalError("Existing-session classifier regressed") }
            await service.configure(reports: [offline], replies: [.init(result: .init(error: PortalError.authenticationUncertain), submitted: true)])
            model.connect(); try await finishManual(model)
            check(originalHolds.allSatisfy { model.holds.contains($0) } && model.lastAttemptOutcome == .uncertain,
                  "Already-online response never clears an earlier credential refusal")
        }

        // RAM and the journal must agree after cancellation, even after the cooldown expires.
        for boundary in ["cancel", "sleep", "path", "mode", "ownedCancelled"] {
            let (model, service, clock) = fixture(area: .teaching)
            await service.configure(reports: [offline, offline], discoveries: [teaching],
                replies: [boundary == "ownedCancelled" ? .init(result: .cancelled, submitted: true) : accepted])
            await model.runAutomaticCycle()
            if boundary == "ownedCancelled" {
                await model.runAutomaticCycle()
            } else {
                await service.pauseNext(.attempt)
                let running = Task { await model.runAutomaticCycle() }
                try await waitUntil("pending cancellation boundary") { await service.isWaiting(.attempt) }
                switch boundary {
                case "sleep": model.setSleeping(true)
                case "path": model.receivePath(usable: true, interfaces: "offline-fixture", identity: .init(fingerprint: "changed"))
                case "mode": model.setMonitoringMode(.manual)
                default: model.cancelCurrentOperation()
                }
                await service.release(.attempt)
                await running.value
            }
            check(model.holds.contains { $0.reason == .uncertain }, "\(boundary) immediately protects unresolved submission in RAM")
            model.setSleeping(false)
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: .init(fingerprint: "changed"))
            model.setMonitoringMode(.recover)
            clock.advance(120)
            await service.configure(reports: [offline, offline])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().submissions.count == 1, "\(boundary) remains blocked after 90 seconds continuously offline")
            model.setProbeMode(.complete)
            await service.configure(reports: [online])
            model.checkConnection(); try await finishManual(model)
            check(!model.holds.contains { $0.reason == .uncertain }, "\(boundary) settles only with a fresh complete all-pass report")
        }

        // Settling uncertainty must not undo or misreport an independent user pause.
        do {
            let (model, service, _) = fixture(area: .teaching)
            model.pauseAutomaticRecovery()
            await service.configure(reports: [offline], replies: [.init(result: .cancelled, submitted: true)])
            model.connect(); try await finishManual(model)
            check(model.holds.contains { $0.reason == .uncertain } && model.holds.contains { $0.reason == .userPaused },
                  "A cancelled manual request preserves both uncertainty and the automatic user pause")
            await service.configure(reports: [online])
            model.checkConnection(); try await finishManual(model)
            check(model.pausedForAutomatic && model.holds.contains { $0.reason == .userPaused }
                  && !model.holds.contains { $0.reason == .uncertain },
                  "A complete successful check settles uncertainty while keeping the user's pause")
            let submissions = await service.snapshot().submissions.count
            check(!model.events.contains { $0.message.contains("自动重连已恢复") } && submissions == 1,
                  "Read-only verification neither resumes user-paused recovery nor reports it resumed")
        }

        // Use the actual background loop so user pause cancels its task during password-free preflight.
        do {
            let service = RecoveryService()
            await service.configure(reports: [offline, offline], discoveries: [teaching], replies: [accepted])
            await service.pauseNext(.preflight)
            let model = AppModel(client: service, isolated: true,
                isolatedCredentials: Credentials(account: "fixture", password: "fixture"),
                runAutomaticLoopInIsolation: true, loopSleep: { _ in try await Task.sleep(for: .milliseconds(5)) })
            model.area = .teaching
            model.setMonitoringMode(.recover)
            model.receivePath(usable: true, interfaces: "offline-fixture", identity: .init(fingerprint: "fixture"))
            try await waitUntil("background preflight") { await service.isWaiting(.preflight) }
            model.pauseAutomaticRecovery()
            await service.release(.preflight)
            try await Task.sleep(for: .milliseconds(20))
            check(await service.snapshot().submissions.isEmpty, "Pausing automatic preflight prevents password submission")
            check(model.holds.contains { $0.reason == .userPaused }, "Pause keeps the user's hold")
            model.setMonitoringMode(.manual)
        }
        do {
            let (model, service, _) = fixture(area: .teaching)
            await service.configure(reports: [offline, online], replies: [accepted])
            await service.pauseNext(.attempt)
            model.connect()
            try await waitUntil("manual login") { await service.isWaiting(.attempt) }
            model.pauseAutomaticRecovery()
            check(model.busy, "Pausing automatic recovery preserves an explicit manual login")
            await service.release(.attempt); try await finishManual(model)
            check(model.lastAttemptOutcome == .success, "Manual login retains ownership after automatic pause")
        }

        // Late discovery and late rejected/success results cannot mutate a newer operation.
        for stage in [RecoveryService.Stage.discovery, .attempt] {
            for late in [areaRejected, accepted] {
                let (model, service, clock) = fixture()
                await service.configure(reports: [offline, offline], discoveries: [teaching], replies: [late])
                await model.runAutomaticCycle()
                await service.pauseNext(stage)
                let background = Task { await model.runAutomaticCycle() }
                try await waitUntil("suspended \(stage)") { await service.isWaiting(stage) }
                model.cancelCurrentOperation()
                await service.configure(reports: [online])
                model.checkConnection(); try await finishManual(model)
                let summary = model.probeSummary
                let checkedAt = model.lastCheckedAt
                let eventCount = model.events.count
                await service.release(stage)
                // Await the real operation completion, even if the fake transport ignored cancellation.
                await background.value
                let snapshot = await service.snapshot()
                check(model.isOnline && model.probeSummary == summary && model.lastCheckedAt == checkedAt,
                      "Cancelled \(stage) / \(late.result.outcome) cannot overwrite newer online evidence")
                check(model.holds.isEmpty && model.events.count == eventCount && model.lastAttemptOutcome == nil,
                      "Cancelled \(stage) / \(late.result.outcome) cannot create holds, success releases or stale events")
                check(snapshot.submissions.count == (stage == .attempt ? 1 : 0),
                      "Cancelled \(stage) / \(late.result.outcome) never starts a later submission")
                checkRecover(model, "Cancellation preserves recover")
                if stage == .attempt {
                    check(model.cooldownRemaining != nil, "Cancelling a submitted request retains its cooldown")
                    await service.configure(reports: [offline, offline])
                    await model.runAutomaticCycle(); await model.runAutomaticCycle()
                    check(await service.snapshot().submissions.count == 1, "Late submitted result cannot remove duplicate-submission protection")
                    clock.advance(90)
                    await service.configure(reports: [offline, online], discoveries: [dormitory], replies: [accepted])
                    await model.runAutomaticCycle()
                    check(await service.snapshot().submissions.count == 2 && model.isOnline,
                          "After cancelled-round cooldown, fresh confirmed dormitory can recover")
                }
            }
        }

        print("\(passed) AUTO-RECOVERY-01 AppModel integration assertions passed; no network or Keychain used.")
    }
}
