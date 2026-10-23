import Foundation

/// Diagnostics contain reviewed enums and bounded values, never system records or request text.
struct DiagnosticEnvironment: Codable, Equatable, Sendable {
    enum Connection: String, Codable, Sendable { case wifi, ethernet, multiple, tunnelHidden, none, unknown }
    enum Proxy: String, Codable, Sendable { case on, off, unknown }
    enum Tunnel: String, Codable, Sendable { case detected, notDetected, unknown }
    var connection: Connection
    var systemProxy: Proxy
    var tun: Tunnel
    var refreshing: Bool
    static let unknown = Self(connection: .unknown, systemProxy: .unknown, tun: .unknown, refreshing: true)
}

/// A notification can invalidate diagnostic freshness before its actor delivery. It does not
/// cancel transport, change authentication ownership, or invalidate historical observations.
final class DiagnosticFreshness: @unchecked Sendable {
    private let lock = NSLock()
    private var revision: UInt64 = 0
    private var applied: UInt64?
    func invalidate() -> UInt64 {
        lock.lock(); defer { lock.unlock() }; revision &+= 1; return revision
    }
    func complete(_ token: UInt64) {
        lock.lock(); defer { lock.unlock() }; if revision == token { applied = token }
    }
    func completeCurrent() {
        lock.lock(); defer { lock.unlock() }; applied = revision
    }
    var current: Bool {
        lock.lock(); defer { lock.unlock() }; return applied == revision
    }
}

struct DiagnosticRecovery: Codable, Equatable, Sendable {
    enum State: String, Codable, Sendable {
        case off, needsAccount, paused, needsTrust, coolingDown, limited, ready, awaitingProof, unknown
    }
    enum Reason: String, Codable, CaseIterable, Sendable { case userPaused, security, uncertain, credentials }
    var state: State
    var readOnly: Bool
    var paused: Bool
    var networkAvailable: Bool
    var networkRemembered: Bool
    var sessionTrusted: Bool
    var holdReasons: [Reason]
    var cooldownRemainingSeconds: Int?
    var valid: Bool {
        holdReasons.count <= 4 && Set(holdReasons).count == holdReasons.count
            && (cooldownRemainingSeconds.map { (1...86_400).contains($0) } ?? true)
    }
    static func seconds(_ value: TimeInterval?) -> Int? {
        guard let value, value.isFinite, value > 0 else { return nil }
        return Int(min(86_400, ceil(value)))
    }
}

struct DiagnosticCampus: Codable, Equatable, Sendable {
    var state: String
    var issue: String
    var valid: Bool {
        ["notChecked", "checking", "confirmed", "unconfirmed"].contains(state)
            && ["none", "dnsTimeout", "dns", "dnsTls", "tls", "connection", "http", "page", "binding"].contains(issue)
    }
    static let notChecked = Self(state: "notChecked", issue: "none")
}

struct DiagnosticPhysical: Codable, Equatable, Sendable {
    static let reasons: Set<String> = ["systemConfigurationUnavailable", "interfaceSnapshotUnavailable",
        "routeSnapshotUnavailable", "addressSnapshotUnavailable", "serviceSnapshotUnavailable",
        "invalidDHCPServer", "invalidDHCPDNS", "invalidServiceRecord", "duplicateService",
        "missingDescriptor", "unsupportedInterface", "inactiveLink", "missingInterfaceIndex",
        "missingSourceAddress", "invalidSourceAddress", "sourceAddressMismatch", "invalidSubnet",
        "invalidGateway", "missingDNS", "invalidDNS", "missingDefaultRoute", "conflictingDefaultRoutes",
        "gatewayMismatch", "unstableSnapshot", "noIPv4Service", "unsupportedPlatform"]
    var physicalInterfaceCount: Int
    var serviceCount: Int
    var candidateCount: Int
    var rejections: [String: Int]
    var valid: Bool {
        [physicalInterfaceCount, serviceCount, candidateCount].allSatisfy { (0...4096).contains($0) }
            && rejections.count <= Self.reasons.count
            && rejections.allSatisfy { Self.reasons.contains($0.key) && (1...4096).contains($0.value) }
    }
}

struct DiagnosticSampleContext: Codable, Equatable, Sendable {
    var phase = "connectivityCheck"
    var operation: String
    var monitoringMode: String
    var area: String
    var startedAt: Date
    var environment: DiagnosticEnvironment
    var valid: Bool {
        phase == "connectivityCheck" && ["manual", "automatic"].contains(operation)
            && ["manual", "observe", "recover"].contains(monitoringMode)
            && ["automatic", "teaching", "dormitory"].contains(area)
            && Self.validDate(startedAt)
    }
    static func validDate(_ date: Date) -> Bool {
        date.timeIntervalSinceReferenceDate.isFinite && abs(date.timeIntervalSinceReferenceDate) < 3_155_760_000
    }
}

struct DiagnosticFailureContext: Codable, Equatable, Sendable {
    static let statuses: Set<String> = ["idle", "checking", "discovering", "loggingIn", "verifying", "online",
        "offline", "confirming", "paused", "coolingDown", "unconfirmed", "needsTrust", "needsAccount",
        "accepted", "rejected", "uncertain", "security", "unavailable", "cancelled", "noNetwork", "sleeping", "unknown"]
    var phase = "operationResult"
    var kind: String
    var result: String
    var startedAt: Date
    var collectedAt: Date
    var operation: String
    var monitoringMode: String
    var area: String
    var environment: DiagnosticEnvironment
    var status: String
    var campusVerification: DiagnosticCampus
    var recovery: DiagnosticRecovery
    var physicalNetwork: DiagnosticPhysical?
    var physicalCollectedAt: Date?
    var valid: Bool {
        phase == "operationResult" && ["connection", "campusVerification"].contains(kind)
            && ["operationFailed", "verificationFailed", "memoryWriteFailed", "identificationUnavailable"].contains(result)
            && ["manual", "automatic"].contains(operation)
            && ["manual", "observe", "recover"].contains(monitoringMode)
            && ["automatic", "teaching", "dormitory"].contains(area)
            && Self.statuses.contains(status) && campusVerification.valid && recovery.valid
            && DiagnosticSampleContext.validDate(startedAt) && DiagnosticSampleContext.validDate(collectedAt)
            && collectedAt >= startedAt && (physicalNetwork?.valid ?? true)
            && (physicalCollectedAt.map(DiagnosticSampleContext.validDate) ?? true)
    }
}

/// Only reviewed protocol identifiers survive event persistence and export.
/// No account, client address, challenge, query string or server text belongs here.
struct DiagnosticAuthentication: Codable, Equatable, Sendable {
    var area: String
    var stage: String
    var path: String
    var httpStatus: Int?
    var acID: String?
    var result: String
    var code: String
    var serverCode: String?
    var saved: Bool

    static let results: Set<String> = ["success", "rejected", "invalidCredentials", "uncertain", "security",
                                       "unavailable", "challengeRefused", "cancelled"]
    static let codes: Set<String> = ["accepted", "explicitRejection", "missingStatus", "conflictingStatus", "invalidStatus",
        "existingSession", "malformedResponse", "transportFailure", "httpFailure", "securityFailure", "cancelled"]
    static let serverCodes: Set<String> = ["E2553", "E2531", "E2833", "nas_ip_error", "ac_id_error",
                                           "ip_already_online_error", "unknown"]
    var valid: Bool {
        guard ["teaching", "dormitory"].contains(area), Self.results.contains(result), Self.codes.contains(code),
              httpStatus.map({ (100...599).contains($0) }) ?? true,
              serverCode.map({ Self.serverCodes.contains($0) }) ?? true else { return false }
        if let acID {
            guard (1...8).contains(acID.utf8.count), acID.utf8.allSatisfy({ (48...57).contains($0) }),
                  let number = Int(acID), number > 0, String(number) == acID else { return false }
        }
        switch stage {
        case "homepage": return path == "/srun_portal_pc" && area == "teaching"
        case "challenge": return path == "/cgi-bin/get_challenge" && area == "teaching"
        case "submission": return (path == "/cgi-bin/srun_portal" && area == "teaching")
                || (path == "/eportal/portal/login" && area == "dormitory")
        default: return false
        }
    }
}
