import Foundation

// Scoped authentication holds, campus planning and rejection
// classification. Pure value logic mirrored 1:1 by windows/SZUConnect/core (policy.cpp) and
// verified against shared/fixtures/auto-recovery-scenarios.json on both platforms.

/// Declaration order is display priority (userPaused first).
enum HoldReason: Int, Comparable, CaseIterable, Codable, Sendable {
    case userPaused = 0, security, uncertain, credentials
    static func < (lhs: HoldReason, rhs: HoldReason) -> Bool { lhs.rawValue < rhs.rawValue }
    var key: String {
        switch self {
        case .userPaused: "userPaused"
        case .security: "security"
        case .uncertain: "uncertain"
        case .credentials: "credentials"
        }
    }
}

enum HoldOrigin: String, Codable, Sendable { case explicitArea, candidate, global, user }
enum DiscoveryState: String, Codable, Sendable { case confirmed, unconfirmed, notChecked }

/// Anonymous fixed-stage failure; no server text, network identity or URL enters diagnostics.
enum CampusVerificationIssue: String, Codable, Sendable {
    case none, dnsTimeout, dns, dnsTls, tls, connection, http, page, binding
}

struct CampusDiscovery: Equatable, Sendable {
    var teaching: DiscoveryState
    var dormitory: DiscoveryState
    // A fresh fixed HTTPS page supplied a SRun configuration for the exact bound
    // source. This selects a candidate; it is neither permission nor login success.
    var teachingClientBound = false
    var issue: CampusVerificationIssue = .none
}

enum RejectionKind: String, Codable, Sendable { case password, account, areaMismatch, unspecified }

enum LoginOutcome: String, Codable, Sendable {
    case success, rejected, invalidCredentials, uncertain, security, unavailable, challengeRefused, cancelled
    /// shared/status-fixtures.json status vocabulary for a finished attempt.
    var engineStatus: String {
        switch self {
        case .success: "accepted"
        case .rejected, .invalidCredentials: "rejected"
        case .uncertain: "uncertain"
        case .security: "security"
        case .cancelled: "cancelled"
        case .unavailable, .challengeRefused: "unavailable"
        }
    }
}

/// Fixed, credential-free authentication evidence. Never put response text or request URLs here.
struct AuthDiagnostic: Sendable {
    enum Stage: String, Sendable { case homepage, challenge, submission }
    enum Path: String, Sendable {
        case homepage = "/srun_portal_pc"
        case challenge = "/cgi-bin/get_challenge"
        case teachingSubmit = "/cgi-bin/srun_portal"
        case dormitorySubmit = "/eportal/portal/login"
    }
    enum Code: String, Sendable {
        case accepted, explicitRejection, missingStatus, conflictingStatus, invalidStatus, existingSession
        case malformedResponse, transportFailure, httpFailure, securityFailure, cancelled
    }
    enum ServerCode: String, Sendable {
        case e2553 = "E2553", e2531 = "E2531", e2833 = "E2833"
        case nasIpError = "nas_ip_error", acIdError = "ac_id_error", alreadyOnline = "ip_already_online_error", unknown
    }
    let stage: Stage
    let path: Path
    let httpStatus: Int?
    let acID: String?
    let code: Code
    let serverCode: ServerCode?

    init(stage: Stage, path: Path, httpStatus: Int? = nil, acID: String? = nil, code: Code,
         serverCode: ServerCode? = nil) {
        self.stage = stage
        self.path = path
        self.httpStatus = httpStatus.flatMap { (100...599).contains($0) ? $0 : nil }
        self.acID = acID.flatMap { value in
            guard (1...8).contains(value.utf8.count), value.utf8.allSatisfy({ (48...57).contains($0) }),
                  let number = Int(value), number > 0 else { return nil }
            return String(number)
        }
        self.code = code
        self.serverCode = serverCode
    }
}

/// Result of one credential attempt against one explicit area. `message` never contains credentials.
struct LoginAttempt: Sendable {
    var outcome: LoginOutcome
    var rejection: RejectionKind = .unspecified
    var error: PortalError?
    var message: String?
    var diagnostic: AuthDiagnostic?

    init(outcome: LoginOutcome, rejection: RejectionKind = .unspecified, error: PortalError? = nil, message: String? = nil,
         diagnostic: AuthDiagnostic? = nil) {
        self.outcome = outcome
        self.rejection = rejection
        self.error = error
        self.message = message
        self.diagnostic = diagnostic
    }
    static func success(_ message: String) -> LoginAttempt { LoginAttempt(outcome: .success, message: message) }
    static let cancelled = LoginAttempt(outcome: .cancelled)
    /// These classified results precede the credential-bearing request. Every
    /// fallback and journal decision uses this boundary, not a second error list.
    var submissionNotStarted: Bool { outcome == .unavailable || outcome == .challengeRefused }

    /// Classifies an error thrown by a transport. Unknown errors are "unavailable" and never
    /// surface their description (it may contain a credential-bearing URL).
    init(error: Error) {
        if error is CancellationError { self.init(outcome: .cancelled); return }
        guard let portal = error as? PortalError else { self.init(outcome: .unavailable); return }
        let outcome: LoginOutcome
        switch portal {
        case .invalidCredentials: outcome = .invalidCredentials
        case .rejected: outcome = .rejected
        case .authenticationUncertain: outcome = .uncertain
        case .tls, .forbiddenTarget, .cryptography, .redirect, .responseTooLarge, .protocolConflict: outcome = .security
        default: outcome = .unavailable
        }
        self.init(outcome: outcome, error: portal, message: portal.localizedDescription)
    }
}

struct AuthenticationHold: Equatable, Identifiable, Codable, Sendable {
    var reason: HoldReason
    var area: CampusArea?          // nil: every area
    var generation: UInt64?        // nil: any credentials
    var epoch: UInt64?             // nil: survives network changes ("sticky")
    var origin: HoldOrigin
    var rejection: String = ""

    var id: String { "\(reason.key)|\(area?.rawValue ?? "all")|\(generation.map(String.init) ?? "-")|\(epoch.map(String.init) ?? "-")" }
    var sticky: Bool { epoch == nil }

    func applies(to target: CampusArea, generation current: UInt64?, epoch currentEpoch: UInt64) -> Bool {
        if let area, area != target { return false }
        if let generation, generation != current { return false }
        if let epoch, epoch != currentEpoch { return false }
        return true
    }
}

struct HoldBook: Sendable {
    static let stickyAfter = 2
    private(set) var holds: [AuthenticationHold] = []
    private var candidateCounts: [String: Int] = [:]

    private static func countKey(_ area: CampusArea, _ generation: UInt64) -> String { "\(area.rawValue)#\(generation)" }

    mutating func add(_ hold: AuthenticationHold) {
        holds.removeAll { $0.reason == hold.reason && $0.area == hold.area && $0.generation == hold.generation && $0.epoch == hold.epoch }
        holds.append(hold)
    }

    /// unavailable / challengeRefused / cancelled never hold; security and uncertain are global;
    /// invalid credentials hold the saved generation everywhere; an unsaved draft never speaks for
    /// the saved account; a non-password refusal of an automatically chosen candidate holds that
    /// candidate on this network only, becoming sticky after `stickyAfter` refusals.
    @discardableResult
    mutating func recordFailure(automaticChoice: Bool, area: CampusArea, generation: UInt64?, epoch: UInt64,
                                attempt: LoginAttempt) -> AuthenticationHold? {
        let hold: AuthenticationHold
        switch attempt.outcome {
        case .success, .unavailable, .challengeRefused, .cancelled:
            return nil
        case .security:
            hold = AuthenticationHold(reason: .security, origin: .global)
        case .uncertain:
            hold = AuthenticationHold(reason: .uncertain, origin: .global)
        case .invalidCredentials:
            guard let generation else { return nil }
            hold = AuthenticationHold(reason: .credentials, generation: generation, origin: .explicitArea, rejection: "invalidCredentials")
        case .rejected:
            guard let generation else { return nil }
            if automaticChoice && attempt.rejection != .password {
                let key = Self.countKey(area, generation)
                let count = (candidateCounts[key] ?? 0) + 1
                candidateCounts[key] = count
                hold = AuthenticationHold(reason: .credentials, area: area, generation: generation,
                                          // An area mismatch describes the network, never the credentials: it must not outlive the network.
                                          epoch: (count >= Self.stickyAfter && attempt.rejection != .areaMismatch) ? nil : epoch, origin: .candidate,
                                          rejection: attempt.rejection.rawValue)
            } else {
                hold = AuthenticationHold(reason: .credentials, area: area, generation: generation,
                                          origin: automaticChoice ? .candidate : .explicitArea, rejection: attempt.rejection.rawValue)
            }
        }
        add(hold)
        return hold
    }

    @discardableResult
    mutating func recordSuccess(area: CampusArea, generation: UInt64?, epoch: UInt64) -> [AuthenticationHold] {
        guard let generation else { return [] }
        var released: [AuthenticationHold] = []
        var kept: [AuthenticationHold] = []
        for hold in holds {
            let sameGeneration = hold.generation == generation
            let release = (sameGeneration && hold.area == area && hold.reason != .userPaused)
                || (hold.area == nil && hold.generation == nil && (hold.reason == .security || hold.reason == .uncertain))
                || (sameGeneration && hold.origin == .candidate && hold.epoch == epoch)
                || (sameGeneration && hold.area == nil && hold.reason == .credentials)
            if release { released.append(hold) } else { kept.append(hold) }
        }
        holds = kept
        for hold in released { if let area = hold.area { candidateCounts[Self.countKey(area, generation)] = nil } }
        candidateCounts[Self.countKey(area, generation)] = nil
        return released
    }

    /// Verified connectivity settles an uncertain submission (never a rejection or a security stop).
    @discardableResult
    mutating func settleUncertain() -> Bool {
        let before = holds.count
        holds.removeAll { $0.reason == .uncertain }
        return holds.count != before
    }

    mutating func resume() {
        holds.removeAll()
        candidateCounts.removeAll()
    }

    mutating func pauseByUser() { add(AuthenticationHold(reason: .userPaused, origin: .user)) }

    mutating func credentialsChanged(to generation: UInt64) {
        holds.removeAll { $0.generation != nil && $0.generation != generation }
        candidateCounts = candidateCounts.filter { $0.key.hasSuffix("#\(generation)") }
    }

    mutating func pathChanged(to epoch: UInt64) {
        holds.removeAll { $0.epoch != nil && $0.epoch != epoch }
    }

    private static func strongest(_ found: [AuthenticationHold]) -> AuthenticationHold? { found.min { $0.reason < $1.reason } }

    func blocking(_ area: CampusArea, generation: UInt64?, epoch: UInt64) -> AuthenticationHold? {
        Self.strongest(holds.filter { $0.applies(to: area, generation: generation, epoch: epoch) })
    }

    func globalHold(generation: UInt64?, epoch: UInt64) -> AuthenticationHold? {
        Self.strongest(holds.filter { $0.area == nil && $0.applies(to: .teaching, generation: generation, epoch: epoch) })
    }

    var strongest: AuthenticationHold? { Self.strongest(holds) }
}

/// One submission per round. Manual rounds are never blocked by holds and never need trust.
enum CampusPlan: Equatable, Sendable {
    case submit(CampusArea), paused, unconfirmed, needsTrust

    static func decide(manual: Bool, choice: CampusArea, discovery: CampusDiscovery, trusted: Bool, holds: HoldBook,
                       generation: UInt64?, epoch: UInt64, memory: MatchLevel = .none, ambiguous: Bool = false) -> CampusPlan {
        let teachingOK = discovery.teaching == .confirmed
        let dormitoryOK = discovery.dormitory == .confirmed
        // Remembered-network eligibility with fresh campus proof does not require session trust.
        let remembered = memoryVerdict(level: memory, discovery: discovery, ambiguous: ambiguous) == .authorized
        func blocked(_ area: CampusArea) -> Bool { !manual && holds.blocking(area, generation: generation, epoch: epoch) != nil }
        switch choice {
        case .teaching:
            if !manual && !teachingOK { return .unconfirmed }
            return blocked(.teaching) ? .paused : .submit(.teaching)
        case .dormitory:
            if !manual && !((teachingOK && dormitoryOK) || remembered) { return .unconfirmed }
            if blocked(.dormitory) { return .paused }
            if !manual && !(trusted || remembered) { return .needsTrust }
            return .submit(.dormitory)
        case .automatic:
            if !teachingOK {
                // A remembered network with a fresh dormitory proof does not need the teaching service to be up.
                if remembered { return blocked(.dormitory) ? .paused : .submit(.dormitory) }
                return .unconfirmed
            }
            if dormitoryOK {
                if !blocked(.dormitory) { return (manual || trusted || remembered) ? .submit(.dormitory) : .needsTrust }
                if !blocked(.teaching) { return .submit(.teaching) }
                return .paused
            }
            return blocked(.teaching) ? .paused : .submit(.teaching)
        }
    }
}

/// A fixed probe's redirect selects a candidate, never campus identity or permission.
/// Only evidence from this operation's bound physical probe is considered.
enum CampusRoutingResolution: Equatable, Sendable {
    case selected(CampusArea, allowUnsentFallback: Bool)
    case unconfirmed
    case conflictingHints
    case needsSelection

    static func resolve(choice: CampusArea, report: ConnectivityReport, discovery: CampusDiscovery,
                        physicallyBound: Bool, preferTeaching: Bool = false) -> CampusRoutingResolution {
        guard choice == .automatic else { return .selected(choice, allowUnsentFallback: false) }
        guard physicallyBound else { return .selected(.automatic, allowUnsentFallback: true) }
        let apple = report.appleResult.campusAreaHint
        let microsoft = report.microsoftResult.campusAreaHint
        if let apple, let microsoft, apple != microsoft { return .conflictingHints }
        guard let hint = apple ?? microsoft else {
            if discovery.teaching == .confirmed, discovery.teachingClientBound {
                return .selected(.teaching, allowUnsentFallback: false)
            }
            // A previous teaching choice is a candidate, never fresh campus proof.
            // Current physical hints (including conflicts) take priority over it.
            if preferTeaching, discovery.teaching == .confirmed {
                return .selected(.teaching, allowUnsentFallback: false)
            }
            // Reachability of two homepages cannot identify the controller serving
            // this attachment. Do not send a password to the dormitory by default.
            if discovery.teaching == .confirmed, discovery.dormitory == .confirmed { return .needsSelection }
            return .selected(.automatic, allowUnsentFallback: true)
        }
        guard discovery.teaching == .confirmed else { return .unconfirmed }
        switch hint {
        case .teaching:
            return .selected(.teaching, allowUnsentFallback: false)
        case .dormitory:
            guard discovery.dormitory == .confirmed else { return .unconfirmed }
            return .selected(.dormitory, allowUnsentFallback: false)
        }
    }
}

/// Bounded selection evidence only. No URL, query, address, account or server text.
struct CampusRoutingDiagnostic: Sendable {
    enum Source: String, Sendable {
        case explicit, physicalHint, clientConfig, rememberedTeaching, legacy
        case unconfirmed, conflictingHints, needsSelection
    }
    let choice: CampusArea
    let physicallyBound: Bool
    let preferTeaching: Bool
    let apple: CampusPortalHint?
    let microsoft: CampusPortalHint?
    let discovery: CampusDiscovery
    let resolution: CampusRoutingResolution

    init(choice: CampusArea, report: ConnectivityReport, discovery: CampusDiscovery,
         discoveryChecked: Bool, physicallyBound: Bool, preferTeaching: Bool) {
        self.choice = choice
        self.physicallyBound = physicallyBound
        self.preferTeaching = preferTeaching
        apple = report.appleResult.campusAreaHint
        microsoft = report.microsoftResult.campusAreaHint
        self.discovery = discoveryChecked ? discovery : CampusDiscovery(teaching: .notChecked, dormitory: .notChecked)
        resolution = CampusRoutingResolution.resolve(choice: choice, report: report, discovery: discovery,
            physicallyBound: physicallyBound, preferTeaching: preferTeaching)
    }

    var source: Source {
        switch resolution {
        case .selected(let area, _):
            if choice != .automatic { return .explicit }
            if physicallyBound, apple != nil || microsoft != nil { return .physicalHint }
            if physicallyBound, discovery.teachingClientBound, area == .teaching { return .clientConfig }
            if physicallyBound, preferTeaching, area == .teaching { return .rememberedTeaching }
            return .legacy
        case .unconfirmed: return .unconfirmed
        case .conflictingHints: return .conflictingHints
        case .needsSelection: return .needsSelection
        }
    }

    var summary: String {
        let selection: String
        if case .selected(let area, _) = resolution { selection = area.rawValue }
        else { selection = "none" }
        return ["route choice=\(choice.rawValue)", "bound=\(physicallyBound ? "yes" : "no")",
            "teaching=\(discovery.teaching.rawValue)", "dormitory=\(discovery.dormitory.rawValue)",
            "apple=\(apple?.rawValue ?? "none")", "microsoft=\(microsoft?.rawValue ?? "none")",
            "preferred=\(preferTeaching ? "teaching" : "none")", "selection=\(selection)", "source=\(source.rawValue)"].joined(separator: " ")
    }
}

extension CampusDiscovery {
    var teachingConfirmed: Bool { teaching == .confirmed }
    var dormitoryConfirmed: Bool { dormitory == .confirmed }

    /// Service scope for a read-only check or memory. This never means logged in.
    func confirmedCandidate(for choice: CampusArea) -> CampusArea? {
        guard teachingConfirmed else { return nil }
        if choice == .teaching || (choice == .automatic && teachingClientBound) { return .teaching }
        if dormitoryConfirmed { return .dormitory }
        return choice == .automatic ? .teaching : nil
    }
}

func memoryVerdict(level: MatchLevel, discovery: CampusDiscovery, ambiguous: Bool) -> MemoryVerdict {
    memoryVerdict(level: level, teachingConfirmed: discovery.teachingConfirmed, dormitoryConfirmed: discovery.dormitoryConfirmed, ambiguous: ambiguous)
}

extension PortalWire {
    /// Only allowlisted protocol tokens are retained; all other server text is discarded.
    static func serverCode(_ object: [String: Any]) -> AuthDiagnostic.ServerCode? {
        let fields = ["error_msg", "ploy_msg", "error", "res", "suc_msg", "msg", "message"]
        let values = fields.compactMap { object[$0] as? String }
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty && $0.lowercased() != "ok" }
        guard !values.isEmpty else { return nil }
        let known: [(String, AuthDiagnostic.ServerCode)] = [
            ("e2553", .e2553), ("e2531", .e2531), ("e2833", .e2833),
            ("nas_ip_error", .nasIpError), ("ac_id_error", .acIdError), ("ip_already_online_error", .alreadyOnline)
        ]
        let matched = known.filter { token, _ in
            let pattern = #"(?<![A-Za-z0-9_])"# + token + #"(?![A-Za-z0-9_])"#
            return values.contains { $0.range(of: pattern, options: [.regularExpression, .caseInsensitive]) != nil }
        }
        return matched.count == 1 ? matched[0].1 : .unknown
    }

    /// Classifies a portal refusal (shared/fixtures/rejection-classification-cases.json).
    static func rejectionKind(_ object: [String: Any]) -> RejectionKind {
        let fields = ["error_msg", "ploy_msg", "error", "res", "suc_msg", "msg", "message"]
        let joined = fields.compactMap { object[$0] as? String }.filter { !$0.isEmpty }.map { $0.lowercased() }.joined(separator: " | ")
        let password = ["e2553", "password is error", "password error", "ldap auth error", "密码错误", "密码不正确", "账号或密码错误", "用户名或密码错误"]
        let account = ["e2531", "user not found", "userid error", "no such user", "用户不存在", "账号不存在", "账户不存在"]
        let area = ["e2833", "dhcp table", "not in the dhcp", "不在认证范围", "ip地址不在", "ip 地址不在", "不允许在此区域", "nas_ip_error", "ac_id_error"]
        if password.contains(where: { joined.contains($0) }) { return .password }
        if account.contains(where: { joined.contains($0) }) { return .account }
        if area.contains(where: { joined.contains($0) }) { return .areaMismatch }
        return .unspecified
    }
}


/// Local safety state only. No credentials, network identity, URL, or server text is persisted.
/// Network-scoped candidate holds and network trust deliberately expire at process boundaries.
struct RecoveryJournal: Codable {
    var schema = 1
    var generation: UInt64
    var holds: [AuthenticationHold]
    var notBefore: Date?

    init(generation: UInt64, holds: [AuthenticationHold], notBefore: Date?, pendingSubmission: Bool) {
        self.generation = generation
        self.holds = holds.filter { $0.epoch == nil }
        self.notBefore = notBefore
        if pendingSubmission && !self.holds.contains(where: { $0.reason == .uncertain }) {
            self.holds.append(AuthenticationHold(reason: .uncertain, origin: .global))
        }
    }
    func encoded() throws -> Data {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(self)
    }
    static func decode(_ data: Data, now: Date) throws -> RecoveryJournal {
        guard data.count <= 65_536 else { throw CocoaError(.fileReadTooLarge) }
        var value = try JSONDecoder().decode(Self.self, from: data)
        guard value.schema == 1, value.generation > 0, value.holds.count <= 32,
              value.holds.allSatisfy({ $0.epoch == nil && $0.area != .automatic }) else { throw CocoaError(.coderReadCorrupt) }
        if let deadline = value.notBefore {
            guard deadline.timeIntervalSince1970.isFinite else { throw CocoaError(.coderReadCorrupt) }
            // A wall-clock correction cannot extend a 90-second cooldown indefinitely.
            value.notBefore = deadline > now ? min(deadline, now.addingTimeInterval(90)) : nil
        }
        return value
    }
    static func read(_ url: URL, now: Date) throws -> RecoveryJournal? {
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let handle = try FileHandle(forReadingFrom: url); defer { try? handle.close() }
        return try decode(handle.read(upToCount: 65_537) ?? Data(), now: now)
    }
    func write(_ url: URL) throws {
        try encoded().write(to: url, options: [.atomic, .completeFileProtection])
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }
}
