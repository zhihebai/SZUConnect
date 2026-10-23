import Foundation

/// Pure admission, retry and ownership state. No system services or network side effects.
/// Authentication holds are scoped by area, credential generation and network epoch.
/// See RecoveryHolds.swift and docs/AUTO-RECOVERY-01.md.
/// RAM-only physical interface identity. IP, SSID and tunnel changes never create a new scope.
struct PhysicalAuthenticationScope: Hashable {
    let kind: String
    let interface: String
    var valid: Bool { ["wifi", "ethernet"].contains(kind) && !interface.isEmpty }
}

struct ConnectionPolicy {
    enum Kind { case manual, automatic }
    enum AuthenticationBlock: String {
        case paused, credentials, security, uncertain
        init(_ reason: HoldReason) {
            switch reason {
            case .userPaused: self = .paused
            case .credentials: self = .credentials
            case .security: self = .security
            case .uncertain: self = .uncertain
            }
        }
        var holdReason: HoldReason {
            switch self {
            case .paused: .userPaused
            case .credentials: .credentials
            case .security: .security
            case .uncertain: .uncertain
            }
        }
        var message: String {
            switch self {
            case .paused: SharedCopy.t("hold.userPaused.detail")
            case .credentials: SharedCopy.t("hold.credentials.detail")
            case .security: SharedCopy.t("hold.security.detail")
            case .uncertain: SharedCopy.t("hold.uncertain.detail")
            }
        }
    }
    struct Operation: Equatable {
        let id = UUID()
        let kind: Kind
    }

    var available = false
    var foreground: Bool
    var automaticEnabled = false
    private(set) var active: Operation?
    private(set) var failures = 0
    private(set) var consecutiveProbeFailures = 0
    private(set) var currentNetworkAuthorized = false
    private(set) var stableRounds = 0
    private(set) var holds = HoldBook()
    private(set) var authenticationNotBefore: Date?
    private var acceptedScope: PhysicalAuthenticationScope?
    private var acceptedScopeVerified = false
    private var scopedDeadlines: [PhysicalAuthenticationScope: Date] = [:]

    init(foreground: Bool) { self.foreground = foreground }

    /// Strongest hold across all scopes.
    var authenticationBlock: AuthenticationBlock? { holds.strongest.map { AuthenticationBlock($0.reason) } }

    mutating func authorizeCurrentNetwork() {
        guard available, foreground else { return }
        currentNetworkAuthorized = true
    }
    mutating func authorizeAfterManualSuccess(_ operation: Operation) {
        guard owns(operation), operation.kind == .manual else { return }
        authorizeCurrentNetwork()
    }
    mutating func observeProbe(online: Bool) {
        consecutiveProbeFailures = online ? 0 : min(2, consecutiveProbeFailures + 1)
        stableRounds = online ? min(3, stableRounds + 1) : 0
        if online { resetFailures() }
    }
    func mayAuthenticateAutomatically(_ operation: Operation, campusConfirmed: Bool,
                                      requiresLocalAuthorization: Bool = false, now: Date = Date()) -> Bool {
        owns(operation) && operation.kind == .automatic && canRunAutomatic
            && consecutiveProbeFailures >= 2 && campusConfirmed
            && authenticationBlock == nil
            && (authenticationNotBefore == nil || now >= authenticationNotBefore!)
            && (!requiresLocalAuthorization || currentNetworkAuthorized)
    }

    mutating func restoreRecovery(_ journal: RecoveryJournal) {
        for hold in journal.holds { holds.add(hold) }
        authenticationNotBefore = journal.notBefore
        acceptedScope = nil; acceptedScopeVerified = false; scopedDeadlines.removeAll()
    }

    // MARK: holds
    /// Add a global hold for the given reason.
    mutating func suspendAuthentication(_ reason: AuthenticationBlock) {
        holds.add(AuthenticationHold(reason: reason.holdReason, origin: reason == .paused ? .user : .global))
    }
    mutating func pauseByUser() { holds.pauseByUser() }
    @discardableResult
    mutating func recordFailure(automaticChoice: Bool, area: CampusArea, generation: UInt64?, epoch: UInt64,
                                attempt: LoginAttempt) -> AuthenticationHold? {
        holds.recordFailure(automaticChoice: automaticChoice, area: area, generation: generation, epoch: epoch, attempt: attempt)
    }
    @discardableResult
    mutating func recordSuccess(area: CampusArea, generation: UInt64?, epoch: UInt64) -> [AuthenticationHold] {
        holds.recordSuccess(area: area, generation: generation, epoch: epoch)
    }
    mutating func credentialsChanged(to generation: UInt64) { holds.credentialsChanged(to: generation) }
    mutating func pathChanged(to epoch: UInt64) { holds.pathChanged(to: epoch) }
    mutating func resumeAuthentication() {
        holds.resume()
        // Recovery may clear holds, but an already-sent request can still be settling.
        // Preserve its deadline so clicking resume cannot bypass duplicate-submission protection.
        resetFailures()
        resetProbeHistory()
    }
    /// Verified connectivity settles an uncertain submission: the network works, so the
    /// duplicate-submission risk behind that pause no longer applies. Credential and security
    /// holds are never released this way, and the submission cooldown is left untouched.
    @discardableResult mutating func settleUncertainAuthentication() -> Bool { holds.settleUncertain() }

    struct CooldownState {
        let notBefore: Date?
        let scope: PhysicalAuthenticationScope?
        let verified: Bool
    }
    var cooldownState: CooldownState {
        CooldownState(notBefore: authenticationNotBefore, scope: acceptedScope, verified: acceptedScopeVerified)
    }
    mutating func restoreCooldownState(_ state: CooldownState) {
        authenticationNotBefore = state.notBefore; acceptedScope = state.scope; acceptedScopeVerified = state.verified
    }
    mutating func authenticationAttemptStarted(now: Date = Date()) {
        // A cancelled in-flight submission may still have reached the server.
        authenticationNotBefore = now.addingTimeInterval(90)
        acceptedScope = nil; acceptedScopeVerified = false
    }
    mutating func authenticationAccepted(now: Date = Date(), clearBlock: Bool = true,
                                          scope: PhysicalAuthenticationScope? = nil) {
        authenticationNotBefore = now.addingTimeInterval(90)
        acceptedScope = scope?.valid == true ? scope : nil
        acceptedScopeVerified = false
        if clearBlock { holds.resume() }
        resetProbeHistory()
    }
    /// A successful response is insufficient; require a complete, owned, interface-bound connectivity check.
    mutating func verifyAcceptedScope(_ scope: PhysicalAuthenticationScope?, now: Date) {
        guard let scope, scope.valid, scope == acceptedScope, let deadline = authenticationNotBefore else { return }
        scopedDeadlines = scopedDeadlines.filter { $0.value > now }
        // Never evict another interface's active protection to admit a new one.
        guard scopedDeadlines[scope] != nil || scopedDeadlines.count < 16 else { return }
        scopedDeadlines[scope] = deadline
        acceptedScopeVerified = true
    }
    func cooldownRemaining(now: Date = Date()) -> TimeInterval? {
        guard let until = authenticationNotBefore, until > now else { return nil }
        return until.timeIntervalSince(now)
    }
    func cooldownRemaining(now: Date, scope: PhysicalAuthenticationScope?) -> TimeInterval? {
        let global = cooldownRemaining(now: now)
        guard global != nil, let scope, scope.valid, let acceptedScope, acceptedScopeVerified else { return global }
        if let until = scopedDeadlines[scope], until > now { return until.timeIntervalSince(now) }
        return scope == acceptedScope ? global : nil
    }
    mutating func resetProbeHistory() { consecutiveProbeFailures = 0 }
    mutating func revokeAuthorization() { currentNetworkAuthorized = false }
    mutating func resetForPathChange() {
        cancel()
        revokeAuthorization()
        resetFailures()
        stableRounds = 0
        // Holds are pruned by epoch (pathChanged); submission cooldown survives path changes.
    }
    var canRunAutomatic: Bool { automaticEnabled && foreground && available }
    var retryDelay: Int { min(300, 30 * (1 << min(failures, 4))) }
    /// `paused`: whether automatic submission is currently paused for the configured area;
    /// nil reports whether any hold is active.
    func nextCheckDelay(constrained: Bool = false, now: Date = Date(), paused: Bool? = nil) -> Int {
        if let until = authenticationNotBefore, until > now { return 15 }
        if paused ?? (authenticationBlock != nil) { return constrained ? 120 : 60 }
        if consecutiveProbeFailures == 1 { return 5 }
        if failures > 0 { return retryDelay }
        if constrained { return 120 }
        return stableRounds < 2 ? 30 : (stableRounds == 2 ? 45 : 60)
    }
    mutating func begin(_ kind: Kind) -> Operation? {
        guard foreground, available, active == nil,
              kind == .manual || automaticEnabled else { return nil }
        let operation = Operation(kind: kind)
        active = operation
        return operation
    }
    func owns(_ operation: Operation) -> Bool { active == operation }
    @discardableResult mutating func finish(_ operation: Operation) -> Bool {
        guard owns(operation) else { return false }
        active = nil
        return true
    }
    mutating func cancel(_ kind: Kind? = nil) {
        if kind == nil || active?.kind == kind { active = nil }
        resetProbeHistory()
    }
    mutating func failed() { failures = min(failures + 1, 4) }
    mutating func resetFailures() { failures = 0 }
}

/// Persistent recovery evidence may receive one bounded round. Two complete transport
/// recoveries, including every selected website, are required before another episode.
/// This is RAM-only scheduling state; campus identity, trust, holds and cooldown stay in ConnectionPolicy.
struct PartialConnectivityRecovery: Sendable {
    private(set) var consecutivePartial = 0
    private(set) var attempted = false
    private var transportSuccesses = 0

    mutating func observe(partial: Bool, transportRecovered: Bool) {
        if transportRecovered {
            consecutivePartial = 0
            transportSuccesses = min(2, transportSuccesses + 1)
            if transportSuccesses >= 2 { attempted = false }
        } else {
            transportSuccesses = 0
            consecutivePartial = partial ? min(2, consecutivePartial + 1) : 0
        }
    }
    var shouldAttempt: Bool { consecutivePartial >= 2 && !attempted }
    var needsConfirmation: Bool { consecutivePartial == 1 && !attempted }
    mutating func noteAttempt() { attempted = true; transportSuccesses = 0 }
    mutating func resetConfirmation() { consecutivePartial = 0; transportSuccesses = 0 }
    mutating func reset() { self = Self() }
}
