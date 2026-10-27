import Foundation
import SwiftUI
import Network
import Security
import CFNetwork
#if os(macOS)
import ServiceManagement
import AppKit
#endif

enum CredentialStore {
    private static var query: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: "local.SZUConnect.credentials",
         kSecAttrAccount as String: "campus-account"]
    }
    static func load() throws -> Credentials? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw StoreError.failed(status) }
        return try JSONDecoder().decode(Credentials.self, from: data)
    }
    static func save(_ credentials: Credentials) throws {
        let data = try JSONEncoder().encode(credentials)
        let attributes: [String: Any] = [kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        let status = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            var q = query
            attributes.forEach { q[$0.key] = $0.value }
            let result = SecItemAdd(q as CFDictionary, nil)
            guard result == errSecSuccess else { throw StoreError.failed(result) }
        } else if status != errSecSuccess { throw StoreError.failed(status) }
    }
    static func delete() throws {
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw StoreError.failed(status) }
    }
    enum StoreError: LocalizedError {
        case failed(OSStatus)
        var errorDescription: String? { SharedCopy.t("account.accessFailed") }
    }
}

protocol ConnectionService: Sendable {
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool
    func resetConnections() async
    func probe() async -> ConnectivityReport
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport
    func confirmedCampusArea(area: CampusArea) async -> CampusArea?
    func login(credentials: Credentials, area: CampusArea) async throws -> String
    /// Discover HTTPS campus identity before checking the dormitory portal.
    func discoverCampus(area: CampusArea) async -> CampusDiscovery
    /// One classified attempt against an explicit area; no internal fallback.
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt
    func attemptLogin(credentials: Credentials, area: CampusArea, requireBoundClientConfiguration: Bool) async -> LoginAttempt
}
extension ConnectionService {
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool { route == nil }
    func resetConnections() async {}
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        let result = await probe()
        return ConnectivityReport(appleResult: result.appleResult, microsoftResult: result.microsoftResult, mode: mode)
    }
    /// Compatibility default for offline services without staged discovery.
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        if area == .teaching {
            let confirmed = await confirmedCampusArea(area: .teaching)
            return CampusDiscovery(teaching: confirmed == nil ? .unconfirmed : .confirmed, dormitory: .notChecked)
        }
        switch await confirmedCampusArea(area: .automatic) {
        case .dormitory?: return CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
        case .some: return CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)
        case nil: return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked)
        }
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        do { return .success(try await login(credentials: credentials, area: area)) }
        catch { return LoginAttempt(error: error) }
    }
    func attemptLogin(credentials: Credentials, area: CampusArea, requireBoundClientConfiguration: Bool) async -> LoginAttempt {
        guard !requireBoundClientConfiguration else { return LoginAttempt(error: PortalError.protocolConflict) }
        return await attemptLogin(credentials: credentials, area: area)
    }
}
extension PortalClient: ConnectionService {}

/// Read-only facts about the user's own network setup. The app never uses or changes them;
/// URLSession ignores these proxy settings; packet-tunnel routing remains system-managed.
enum NetworkEnvironment {
    static func systemProxyEnabled() -> Bool { systemProxyState() ?? false }
    static func systemProxyState() -> Bool? {
        guard let unmanaged = CFNetworkCopySystemProxySettings(),
              let settings = unmanaged.takeRetainedValue() as NSDictionary as? [String: Any] else { return nil }
        return ["HTTPEnable", "HTTPSEnable", "SOCKSEnable", "ProxyAutoConfigEnable"].contains { key in
            (settings[key] as? NSNumber)?.boolValue == true
        }
    }
}

private let defaultTrustReason = SharedCopy.t("guard.trustDetail")

enum AppMaintenanceState: Equatable { case running, stopping, ready }

@MainActor final class AppModel: ObservableObject {
    @Published private(set) var maintenanceState = AppMaintenanceState.running
    var isMaintaining: Bool { maintenanceState != .running }
    @Published private(set) var uninstallRemovesData = false
    var canCollectCrashMetadata: Bool { !sessionReadOnly && !isPreview && !isolated }
    var canUninstallApplication: Bool { !isolated && !isPreview && !sessionReadOnly && !credentialsDeletionInProgress }
    @Published var account = "" { didSet { if account != oldValue { configurationChanged() } } }
    @Published var password = "" { didSet { if password != oldValue { configurationChanged() } } }
    @Published var area: CampusArea = .automatic {
        didSet {
            guard !isMaintaining, area != oldValue else { return }
            defaults?.set(area.rawValue, forKey: "area")
            setVerificationFeedback(nil)
            lastDiscovery = nil
            lastRoutingDiagnostic = nil
            routingDiagnosticEpoch = nil
            configurationChanged()
        }
    }
    @Published var autoReconnect = false
    @Published private(set) var monitoringMode: MonitoringMode = .manual
    @Published private(set) var health = ConnectionHealth()
    @Published var launchAtLogin = false
    @Published var status = SharedCopy.t("tray.state.idle")
    @Published var detail = SharedCopy.t("status.noNetwork.detail")
    @Published var busy = false { didSet { updateWiFiRecoveryContext() } }
    @Published var events: [ConnectionEvent] = []
    @Published var errorMessage: String?
    @Published private(set) var isOnline = false
    @Published private(set) var interfaceSummary = SharedCopy.t("env.access.pending")
    @Published private(set) var probeSummary = SharedCopy.t("status.idle.headline")
    @Published private(set) var authenticationSummary = SharedCopy.t("auto.ready.detail")
    private(set) var lastAuthDiagnosticSummary: String?
    private(set) var lastRoutingDiagnostic: CampusRoutingDiagnostic?
    private var routingDiagnosticEpoch: UInt64?
    var entrySelectionNeeded: Bool {
        area == .automatic && !sessionReadOnly && networkAvailable && !pausedForAutomatic
            && lastInternetCheck?.base.allPassed != true
            && currentCampusRouting == .needsSelection
    }
    @Published private(set) var currentNetworkAuthorized = false
    @Published private(set) var authorizationStatusText = "尚未信任此网络"
    @Published private(set) var authorizationReason = defaultTrustReason
    let isPreview: Bool
    private(set) var legacyUserAtLaunch = false
    @Published private(set) var lastCheckedAt: Date?
    @Published private(set) var nextCheckAt: Date?
    @Published private(set) var recoverySummary = SharedCopy.t("env.access.pending")
    @Published private(set) var automaticRecoveryPaused = false
    @Published private(set) var sessionReadOnly = false
    @Published private(set) var alwaysShowDockIcon = false
    @Published private(set) var dockPreferenceAvailable = true
    @Published private(set) var dockVisibilityError: String?
    private let dockPreferences: UserDefaults?
    /// Plain-language reading of the latest probe round; explanatory only.
    @Published private(set) var lastDiagnosis: ConnectivityDiagnosis?
    @Published private(set) var lastLatencyMilliseconds: Double?
    @Published private(set) var probeMode: ConnectivityProbeMode = .quick
    @Published private(set) var lastProbeMode: ConnectivityProbeMode?
    @Published private(set) var lastProbePassedCount = 0
    @Published private(set) var customWebsiteTargets: [WebsiteTarget] = []
    @Published private(set) var customWebsiteStrategy: WebsiteProbeStrategy = .system
    @Published private(set) var lastInternetCheck: InternetCheckReport? {
        didSet { connectivityResultRevision &+= 1 }
    }
    @Published private(set) var connectivityResultRevision: UInt64 = 0
    @Published private(set) var systemConnectivity: ConnectivityReport?
    @Published private(set) var systemConnectivityChecking = false
    @Published private(set) var systemConnectivityCheckedAt: Date?
    @Published private(set) var networkInformationRefreshing = false
    @Published private(set) var networkInformationFeedback: String?
    @Published private(set) var physicalDiagnostics: CampusPhysicalNetwork.CandidateDiagnostics?
    private var networkInformationTask: Task<Void, Never>?
    private let internetCheckDelivery: (@Sendable (InternetCheckReport) async -> Void)?
    private let networkInformationReader: (@Sendable () async throws -> PathObservation)?
    private let manualNetworkReader: (@Sendable () async throws -> PathObservation)?
    @Published private(set) var manualNetworkReadError: String?
    private var manualSnapshotTask: Task<Void, Never>?
    private var manualSnapshotID: UUID?
    private var manualSnapshotGeneration: UInt64 = 0
    private var manualResumeDeadline: TimeInterval?
    private var configurationRevision: UInt64 = 0
    @Published private(set) var manualActionRevision: UInt64 = 0
    @Published private(set) var credentialsDeletionInProgress = false
    @Published private(set) var credentialsDeletionMessage: String?
    @Published private(set) var credentialsDeletionFailed = false
    private let credentialDeletion: @Sendable () async throws -> Void
    private var systemConnectivityTask: Task<Void, Never>?
    private var systemConnectivityGeneration: UInt64 = 0
    private var systemConnectivityRecheckNeeded = false
    private let systemConnectivityProbe: (@Sendable (ConnectivityProbeMode) async -> ConnectivityReport)?
    @Published private(set) var websiteConfigurationChanged = false
    @Published var websiteConfigurationError: String?
    private let websiteChecker: any WebsiteChecking
    private var websitePreferencesURL: URL?
    private var websitePreferencesCorrupt = false
    @Published private(set) var networkRevision: UInt = 0
    /// A VPN, packet tunnel or TUN-mode proxy owns the default route (macOS evidence).
    @Published private(set) var tunnelActive = false
    @Published private(set) var systemPhysicalLabel: String?
    @Published private(set) var connectedPhysicalLabels: [String] = []
    @Published private(set) var sessionOnlyTrust = false
    @Published private(set) var networkTrustUnavailableReason: String?
    /// The user's system HTTP(S)/SOCKS/PAC proxy is switched on. Campus requests ignore it.
    @Published private(set) var systemProxyActive = false
    /// A manual connect found the network already passing; the user may still submit once.
    @Published private(set) var offerForcedLogin = false
    private let now: () -> Date
    private let monotonicNow: () -> TimeInterval
    private let client: any ConnectionService
    private let defaults: UserDefaults?
    private let isolated: Bool
    private let automaticLoopAllowed: Bool
    private let loopSleep: @Sendable (Int) async throws -> Void
    private let loopFallbackSleep: @Sendable (TimeInterval) async throws -> Void
    private let verificationSleep: @Sendable (Int) async throws -> Void
    private var initialized = false
    private var monitor: NWPathMonitor?
    private var environmentRefreshTask: Task<Void, Never>?
    #if os(iOS)
    private var policy = ConnectionPolicy(foreground: false)
    #else
    private var policy = ConnectionPolicy(foreground: true)
    private var workspaceObservers: [NSObjectProtocol] = []
    #endif
    private var loop: Task<Void, Never>?
    // A cancelled task can still be unwinding. Retain its handle until completion
    // even when a newer round replaces the user-facing handle.
    private var backgroundTasks: [UUID: Task<Void, Never>] = [:]
    private var loopID: UUID?
    private var manual: Task<Void, Never>?
    private var probeOwner: ConnectionPolicy.Operation?
    /// Only a classified credential attempt and its confirmation pin a live route.
    /// A restored pending journal alone must not pin a new read-only probe.
    private var credentialOperation: ConnectionPolicy.Operation?
    @Published private var activeInternetCheck: UUID?
    var physicalConnectivityChecking: Bool { activeInternetCheck != nil }
    private var logURL: URL?
    private var healthURL: URL?
    private var recoveryJournalURL: URL?
    private var lastRecoveryJournal: RecoveryJournal?
    private var recoveryJournalBlocked = false
    private var pendingRecoverySubmission = false
    @Published private var automaticCredentials: Credentials?
    private var constrained = false
    private var sleeping = false
    private var hasPath = false
    private var currentIdentity: NetworkIdentity?
    private var lastLinkActive = false
    private var lastPathInterfaces = ""
    private var transportGeneration: UInt64 = 0
    private var preparedTransportGeneration: UInt64 = 0
    private var authenticationRoute: CampusPhysicalRoute?
    private var requiresPhysicalBinding = false
    // A real path transition gets a bounded recovery burst, never a new login permission.
    private var rapidRecoveryDeadline: Date?
    private var recoveryFullCheckPending = false
    private var sessionAuditDeadline: TimeInterval?
    private var authorizationGraceDeadline: Date?
    private var authorizationGraceTask: Task<Void, Never>?
    // Authentication holds are scoped by saved-credential generation and network epoch.
    private var savedGeneration: UInt64 = 1
    private var networkEpoch: UInt64 = 1
    private var websiteEnvironmentRevision: UInt64 = 0
    private var websitePathIdentity: NetworkIdentity?
    @Published private(set) var holds: [AuthenticationHold] = []
    @Published private(set) var lastDiscovery: CampusDiscovery? { didSet { diagnosticDiscoveryRevision &+= 1 } }
    private var diagnosticDiscoveryRevision: UInt64 = 0
    private let diagnosticFreshness = DiagnosticFreshness()
    private var diagnosticApplyingObservation = false
    private var diagnosticCancellation: UInt64 = 0
    private var diagnosticProxyKnown = false
    private var physicalCollectedAt: Date?
    private var campusServiceSelectionReport: ConnectivityReport?
    private struct CampusServiceContext: Equatable {
        let revision: UInt
        let area: CampusArea
        let memoryGeneration: UInt64
    }
    private var currentCampusServiceContext: CampusServiceContext {
        CampusServiceContext(revision: networkRevision, area: area, memoryGeneration: memoryGeneration)
    }
    struct CampusDirectToolToken: Equatable {
        let id: UUID
        let revision: UInt
        let generation: UInt64
        let bound: Bool
    }
    @Published private(set) var campusDirectHelpSuggested = false
    private var directNetworkFailures = 0
    private var directToolFailures = 0
    private var directToolGeneration: UInt64 = 0
    private var activeDirectToolTokens: Set<UUID> = []
    private var pendingDirectBase: (generation: UInt64, report: ConnectivityReport)?
    private var countedDirectSystemGeneration: UInt64?

    /// Fresh fixed-URL diagnostic sockets. The closure retains only this proven route;
    /// it never shares login cookies, credentials or mutable model state.
    func campusDirectProbe() -> (@Sendable (URL) async -> String)? {
        #if os(macOS)
        guard !isPreview, let route = authenticationRoute else { return nil }
        return { url in
            guard ["https://net.szu.edu.cn/srun_portal_pc", "https://www.apple.com/library/test/success.html"].contains(url.absoluteString) else { return "notRun" }
            let transport = CampusPhysicalTransport(route: route, cookies: false)
            do {
                let (data, response) = try await transport.data(for: URLRequest(url: url), timeout: 8, limit: 1_048_576)
                if Task.isCancelled { return "cancelled" }
                if url.host == "net.szu.edu.cn" {
                    return PortalWire.isCampusHomepage(data, status: response.statusCode, url: url) ? "reachable" : "httpStatus"
                }
                return (200..<300).contains(response.statusCode) ? "reachable" : "httpStatus"
            } catch is CancellationError { return "cancelled" }
            catch let error as CampusPhysicalTransportError {
                switch error {
                case .cancelled: return "cancelled"
                case .timeout: return "timeout"
                case .dns: return "dns"
                case .tls: return "tls"
                default: return "network"
                }
            } catch { return "network" }
        }
        #else
        return nil
        #endif
    }
    func beginCampusDirectToolCheck() -> CampusDirectToolToken {
        let id = UUID()
        let admitted = activeDirectToolTokens.count < 8
        if admitted { activeDirectToolTokens.insert(id) }
        return CampusDirectToolToken(id: id, revision: networkRevision, generation: directToolGeneration,
            bound: admitted && authenticationRoute != nil)
    }
    func finishCampusDirectToolCheck(_ token: CampusDirectToolToken, campusPassed: Bool, cancelled: Bool) {
        guard activeDirectToolTokens.remove(token.id) != nil else { return }
        guard !isMaintaining, !sessionReadOnly, !isPreview, !sleeping, tunnelActive,
              hasPhysicalConnection, token.bound, token.revision == networkRevision,
              token.generation == directToolGeneration else { return }
        if cancelled { cancelCampusDirectToolChecks(); return }
        if campusPassed { clearCampusDirectToolFailures() }
        else { directToolFailures = min(2, directToolFailures + 1) }
        updateCampusDirectSuggestion()
    }
    func cancelCampusDirectToolChecks() {
        clearCampusDirectToolFailures()
        updateCampusDirectSuggestion()
    }
    private func clearCampusDirectToolFailures() {
        directToolGeneration &+= 1
        activeDirectToolTokens.removeAll()
        directToolFailures = 0
    }
    private func resetCampusDirectFailures() {
        directNetworkFailures = 0
        clearCampusDirectToolFailures()
        pendingDirectBase = nil
        countedDirectSystemGeneration = nil
        updateCampusDirectSuggestion()
    }
    private func updateCampusDirectSuggestion() {
        campusDirectHelpSuggested = !sessionReadOnly && !isPreview && !isMaintaining && !sleeping
            && tunnelActive && hasPhysicalConnection && (directNetworkFailures >= 2 || directToolFailures >= 2)
    }
    private static func completedFixedFailure(_ report: ConnectivityReport) -> Bool {
        !report.online && [report.appleResult, report.microsoftResult].allSatisfy {
            $0.state != .cancelled && $0.state != .notCompleted
        }
    }
    private func observeDirectNetworkResult(base: ConnectivityReport? = nil) {
        guard !sessionReadOnly, !isPreview, !isMaintaining, !sleeping,
              tunnelActive, hasPhysicalConnection else { resetCampusDirectFailures(); return }
        if let base {
            if base.online {
                directNetworkFailures = 0
                pendingDirectBase = nil
                // Reject a system result started before this successful round.
                countedDirectSystemGeneration = systemConnectivityGeneration
                updateCampusDirectSuggestion()
                return
            }
            if Self.completedFixedFailure(base) {
                pendingDirectBase = (systemConnectivityGeneration, base)
            } else { pendingDirectBase = nil }
        }
        guard !systemConnectivityChecking, let system = systemConnectivity else { return }
        if system.online {
            directNetworkFailures = 0
            pendingDirectBase = nil
            countedDirectSystemGeneration = systemConnectivityGeneration
        } else if Self.completedFixedFailure(system), countedDirectSystemGeneration != systemConnectivityGeneration,
                  !physicalNetworkIdentified || pendingDirectBase?.generation == systemConnectivityGeneration {
            directNetworkFailures = min(2, directNetworkFailures + 1)
            countedDirectSystemGeneration = systemConnectivityGeneration
            pendingDirectBase = nil
        }
        updateCampusDirectSuggestion()
    }
    private var confirmedCampusServiceContext: CampusServiceContext?
    private var confirmedCampusServiceDeadline: TimeInterval?
    private var campusServiceRetry: (context: CampusServiceContext, after: TimeInterval)?
    @Published private(set) var lastAttemptOutcome: LoginOutcome?
    /// Shared status vocabulary (shared/status-fixtures.json) consumed by StatusPresenter.
    @Published private(set) var engineStatus = "idle"
    // Network memory stores keyed hashes of stable features, never raw values.
    @Published private(set) var rememberedNetworks: [MemoryRecord] = []
    @Published private(set) var memoryUnreadable = false
    @Published private(set) var memoryWriteFailed = false
    private struct VerificationFeedback {
        let key: String
        let area: CampusArea
        var serviceOnly: Bool { key.hasPrefix("campus.verify.") }
    }
    @Published private var verificationFeedback: VerificationFeedback?
    var memoryVerificationMessage: String? {
        guard let feedback = verificationFeedback, feedback.area == area else { return nil }
        // The phase-specific diagnosis belongs to this completed discovery.
        // Keep the generic feedback key for readiness/retry policy, while its
        // visible (and accessible) message explains the actual failure.
        if ["campus.verify.failed", "campus.verify.autoFailed", "memory.verification.needsProof"].contains(feedback.key),
           let issue = lastDiscovery?.issue, issue != .none {
            return SharedCopy.t("campus.verify." + issue.rawValue + "Hint")
        }
        return SharedCopy.t(feedback.key)
    }
    private func setVerificationFeedback(_ key: String?) {
        verificationFeedback = key.map { VerificationFeedback(key: $0, area: area) }
    }
    var campusVerificationFailed: Bool {
        guard let feedback = verificationFeedback, feedback.area == area else { return false }
        return ["campus.verify.failed", "campus.verify.autoFailed"].contains(feedback.key)
    }
    var automaticCampusVerificationRetryPending: Bool {
        monitoringMode == .recover && networkRemembered && campusServiceRetry?.context == currentCampusServiceContext
            && verificationFeedback?.key == "campus.verify.autoFailed"
    }
    @Published private(set) var isVerifyingMemory = false
    private var campusServiceOnlyVerification = false
    private struct CampusVerificationSchedule {
        let wasRunning: Bool
        let deadline: Date?
    }
    private var campusServiceOnlySchedule: CampusVerificationSchedule?
    var campusVerificationInProgress: Bool {
        busy && (isVerifyingMemory || engineStatus == "discovering")
    }
    private var memory: NetworkMemory?
    private var memoryURL: URL?
    private var memoryGeneration: UInt64 = 0
    private var suppressedMemory: Set<String> = []
    private var currentDescriptors: [NetworkDescriptor] = []
    private var partialRecovery = PartialConnectivityRecovery()
    @Published private(set) var partialRecoveryInProgress = false
    var partialRecoveryAttempted: Bool { partialRecovery.attempted }
    var partialRecoveryAwaitingConfirmation: Bool { partialRecovery.needsConfirmation }

    @Published private(set) var networkLabels = NetworkLabels()
    @Published private(set) var networkLabelsUnavailable = false
    @Published private(set) var networkNameError: String?
    private var networkLabelsStore: NetworkLabelsStore?
    private var observedNetworkLabel: NetworkObservedLabel?
    private var hasPhysicalConnection = true
    #if os(macOS)
    let wifiRecovery: WiFiRecoveryController
    #endif

    init(client: any ConnectionService = PortalClient(), isolated: Bool = false,
         readOnly: Bool = ProcessInfo.processInfo.arguments.contains("--observe-only"),
         now: @escaping () -> Date = Date.init,
         monotonicNow: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }, recoveryStore: URL? = nil, isolatedCredentials: Credentials? = nil,
         websiteChecker: (any WebsiteChecking)? = nil, websiteStore: URL? = nil,
         initialWebsiteTargets: [WebsiteTarget] = [], memoryStore: URL? = nil,
         runAutomaticLoopInIsolation: Bool = false, loopSleep: (@Sendable (Int) async throws -> Void)? = nil,
         loopFallbackSleep: (@Sendable (TimeInterval) async throws -> Void)? = nil,
         labelsStore: URL? = nil, isolatedPreferences: UserDefaults? = nil, recordStoreDirectory: URL? = nil,
         isolatedWiFiRecoveryStore: URL? = nil,
         systemConnectivityProbe: (@Sendable (ConnectivityProbeMode) async -> ConnectivityReport)? = nil,
         verificationSleep: (@Sendable (Int) async throws -> Void)? = nil,
         networkInformationReader: (@Sendable () async throws -> PathObservation)? = nil,
         manualNetworkReader: (@Sendable () async throws -> PathObservation)? = nil,
         credentialDeletion: (@Sendable () async throws -> Void)? = nil,
         internetCheckDelivery: (@Sendable (InternetCheckReport) async -> Void)? = nil) {
        let preview = ProcessInfo.processInfo.arguments.contains("--offline-preview")
            || Bundle.main.bundleIdentifier == "local.SZUConnect.preview"
            || Bundle.main.bundleIdentifier?.hasPrefix("local.SZUConnect.preview.") == true
            || Bundle.main.object(forInfoDictionaryKey: "SZUOfflinePreview") as? Bool == true
        #if os(macOS)
        let privateRun = isolated || preview
        let wifiPreferencesURL = isolated && !preview ? isolatedWiFiRecoveryStore
            : privateRun ? nil : FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
                .appendingPathComponent("SZUConnect/wifi-recovery.json")
        self.wifiRecovery = WiFiRecoveryController(preferencesURL: wifiPreferencesURL,
            service: privateRun ? PreviewWiFiRecoveryService() : CoreWLANRecoveryService(),
            startAutomatically: !privateRun && !readOnly, requestsSystemPermission: !privateRun && !readOnly, preferencesReadOnly: readOnly)
        #endif
        self.now = now
        self.internetCheckDelivery = internetCheckDelivery
        self.monotonicNow = monotonicNow
        self.isPreview = preview
        self.client = preview ? PreviewConnectionService() : client
        self.websiteChecker = preview ? PreviewWebsiteChecker() : (websiteChecker ?? (isolated ? UnconfiguredWebsiteChecker() : WebsiteProbe()))
        self.isolated = isolated || preview
        self.networkInformationReader = networkInformationReader
        self.manualNetworkReader = manualNetworkReader
        let privateCredentials = isolated || preview
        self.credentialDeletion = credentialDeletion ?? {
            if !privateCredentials { try await Task.detached(priority: .userInitiated) { try CredentialStore.delete() }.value }
        }
        if preview { self.systemConnectivityProbe = nil }
        else if let systemConnectivityProbe { self.systemConnectivityProbe = systemConnectivityProbe }
        else if isolated { self.systemConnectivityProbe = nil }
        else { self.systemConnectivityProbe = { await PortalClient.probeSystemConnectivity(mode: $0) } }
        self.automaticLoopAllowed = !preview && (!isolated || runAutomaticLoopInIsolation)
        self.loopSleep = loopSleep ?? { seconds in try await Task.sleep(for: .seconds(seconds)) }
        self.loopFallbackSleep = loopFallbackSleep ?? { seconds in try await Task.sleep(for: .seconds(seconds)) }
        let skipVerificationDelays = isolated || preview
        self.verificationSleep = verificationSleep ?? { seconds in
            if !skipVerificationDelays { try await Task.sleep(for: .seconds(seconds)) }
        }
        // Read the user's choices in observe-only mode, but never retain a writable
        // preference store. Injection is limited to isolated, non-preview tests.
        let storedDefaults: UserDefaults? = self.isolated ? (preview ? nil : isolatedPreferences) : .standard
        self.defaults = readOnly ? nil : storedDefaults
        self.sessionReadOnly = readOnly
        // Preview UI preferences use a separate domain; production data is never touched.
        let previewIdentifier = Bundle.main.bundleIdentifier?.hasPrefix("local.SZUConnect.preview") == true
            ? Bundle.main.bundleIdentifier! : "local.SZUConnect.preview"
        let dockStore = preview ? UserDefaults(suiteName: previewIdentifier + ".dock") : storedDefaults
        self.dockPreferences = readOnly ? nil : dockStore
        if let value = dockStore?.object(forKey: "alwaysShowDockIcon") {
            if let enabled = value as? Bool { alwaysShowDockIcon = enabled }
            else {
                dockPreferenceAvailable = false
                dockVisibilityError = SharedCopy.t("settings.dock.unavailable")
            }
        }
        if let defaults = storedDefaults {
            area = CampusArea(rawValue: defaults.string(forKey: "area") ?? "") ?? .automatic
            monitoringMode = MonitoringMode.migrated(raw: defaults.string(forKey: "monitoringMode"),
                legacyAutoReconnect: defaults.bool(forKey: "autoReconnect"))
            autoReconnect = monitoringMode == .recover
            probeMode = ConnectivityProbeMode(rawValue: defaults.string(forKey: "probeMode") ?? "") ?? .quick
            if !self.isolated {
                if !readOnly {
                    do {
                        if let credentials = try CredentialStore.load() {
                            account = credentials.account
                            password = credentials.password
                            automaticCredentials = credentials
                        }
                    } catch { errorMessage = SharedCopy.t("account.readFailed") }
                }
                loadEvents()
            }
        }
        if self.isolated, !preview, let recordStoreDirectory { loadEvents(from: recordStoreDirectory) }
        legacyUserAtLaunch = automaticCredentials != nil || !events.isEmpty || !health.samples.isEmpty
            || ["area", "monitoringMode", "autoReconnect", "appearanceMode", "probeMode"].contains { storedDefaults?.object(forKey: $0) != nil }
        if self.isolated, !preview, let isolatedCredentials {
            account = isolatedCredentials.account; password = isolatedCredentials.password
            automaticCredentials = isolatedCredentials
        }
        if let recoveryStore { loadRecoveryJournal(recoveryStore) }
        if self.isolated { customWebsiteTargets = (try? WebsiteTarget.validated(initialWebsiteTargets)) ?? [] }
        if let websiteStore { loadWebsitePreferences(websiteStore) }
        if let memoryStore { loadNetworkMemory(memoryStore) }
        if let labelsStore { loadNetworkLabels(labelsStore) }
        policy.automaticEnabled = monitoringMode != .manual
        initialized = true
        #if os(macOS)
        wifiRecovery.onWiFiNamePermissionChanged = { [weak self] in
            guard let self, !self.isMaintaining else { return }
            _ = self.trackedBackgroundTask { [weak self] in await self?.refreshNetworkEnvironmentInBackground() }
        }
        updateWiFiRecoveryContext()
        #endif
        if preview {
            let teachingPreview = Bundle.main.object(forInfoDictionaryKey: "SZUPreviewTeachingOnly") as? Bool == true
            if teachingPreview { area = .teaching }
            account = "演示账户"
            password = "preview-only"
            automaticCredentials = Credentials(account: account, password: password)
            monitoringMode = .recover
            autoReconnect = true
            policy.automaticEnabled = true
            memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
            receivePath(usable: true, interfaces: "Wi-Fi en0",
                        identity: NetworkIdentity(fingerprint: "offline-preview"),
                        descriptors: [NetworkDescriptor(kind: "wifi", raw: ["gw": "a4:5e:60:11:22:33", "gwip": "172.30.0.1", "net": "172.30.0.0/20"])])
            if var previewMemory = memory, !teachingPreview {
                _ = previewMemory.remember(currentDescriptors[0], area: "dormitory", proofTeaching: true,
                    proofDormitory: true, now: now(), newID: "0011223344556677")
                _ = previewMemory.remember(NetworkDescriptor(kind: "ethernet", raw: ["gw": "a4:5e:60:44:55:66", "gwip": "172.31.0.1", "net": "172.31.0.0/20"]),
                    area: "dormitory", proofTeaching: true, proofDormitory: true, now: now(), newID: "8899aabbccddeeff")
                memory = previewMemory
                var labels = NetworkLabels()
                labels.updateAutomatic(id: "0011223344556677", displayName: "SZU_WLAN", kind: .wifi, ssid: "SZU_WLAN")
                labels.updateAutomatic(id: "8899aabbccddeeff", displayName: "宿舍有线网络", kind: .ethernet)
                networkLabels = labels
                refreshRemembered()
            }
            status = SharedCopy.t("diagnosis.online.title")
            detail = "离线演示：界面使用模拟数据，不访问真实网络。"
            isOnline = true
            interfaceSummary = "Wi-Fi en0"
            probeSummary = "Apple：通过 24 ms · Microsoft：另一项已通过，停止等待"
            lastProbeMode = .quick
            lastProbePassedCount = 1
            lastCheckedAt = now()
            lastDiagnosis = .online
            lastLatencyMilliseconds = 24
            recoverySummary = "离线演示，操作只使用模拟数据"
            authenticationSummary = "离线演示，不访问真实网络或账户"
            events = [ConnectionEvent(date: Date(), message: "界面预览，未连接真实网络。")]
            lastInternetCheck = InternetCheckReport(base: ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.024), microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick), websites: [], selectedCount: 0)
            if Bundle.main.object(forInfoDictionaryKey: "SZUPreviewPartial") as? Bool == true {
                probeMode = .complete
                lastProbeMode = .complete
                let report = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.024),
                    microsoftResult: .init(state: .redirect, elapsed: 0.041), mode: .complete)
                lastInternetCheck = InternetCheckReport(base: report, websites: [], selectedCount: 0)
                probeSummary = report.summary
                status = "只有部分检测通过"
                detail = "离线演示：部分通过后可以直接尝试登录。"
            }
            return
        }
        guard !self.isolated else { return }
        #if os(macOS)
        launchAtLogin = SMAppService.mainApp.status == .enabled
        let center = NSWorkspace.shared.notificationCenter
        let sleepFreshness = diagnosticFreshness
        workspaceObservers.append(center.addObserver(forName: NSWorkspace.willSleepNotification,
                                                      object: nil, queue: .main) { [weak self] _ in
            _ = sleepFreshness.invalidate()
            Task { @MainActor in self?.setSleeping(true) }
        })
        workspaceObservers.append(center.addObserver(forName: NSWorkspace.didWakeNotification,
                                                      object: nil, queue: .main) { [weak self] _ in
            _ = sleepFreshness.invalidate()
            Task { @MainActor in self?.setSleeping(false) }
        })
        #endif
        systemProxyActive = NetworkEnvironment.systemProxyEnabled()
        let observer = NWPathMonitor()
        monitor = observer
        let freshness = diagnosticFreshness
        observer.pathUpdateHandler = { [weak self] path in
            let token = freshness.invalidate()
            // Captured on the monitor queue; only value types cross to the main actor.
            let observation = PathObservation.capture(path)
            Task { @MainActor [weak self] in
                self?.applyDiagnosticObservation(token: token) { self?.receive(observation) }
            }
        }
        observer.start(queue: DispatchQueue(label: "local.SZUConnect.path"))
        #if os(macOS)
        // Route-only TUN changes may not change NWPath's default physical interface.
        // This refresh reads local metadata only; it sends no network probe or credential.
        environmentRefreshTask = trackedBackgroundTask { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(5)) } catch { return }
                guard let self else { return }
                await self.refreshNetworkEnvironmentInBackground()
            }
        }
        #endif
    }

    deinit {
        backgroundTasks.values.forEach { $0.cancel() }
        networkInformationTask?.cancel()
        manualSnapshotTask?.cancel()
        systemConnectivityTask?.cancel()
        loop?.cancel()
        manual?.cancel()
        authorizationGraceTask?.cancel()
        monitor?.cancel()
        environmentRefreshTask?.cancel()
        #if os(macOS)
        for token in workspaceObservers { NSWorkspace.shared.notificationCenter.removeObserver(token) }
        #endif
    }

    /// Freeze writers before cancellation can resume any suspended callback. The caller
    /// must wait for this barrier before removing data or moving the application.
    /// A failed uninstall stays frozen so retry cannot restart login or recreate files.
    func prepareForAppMaintenance(removeData: Bool = false) async -> Bool {
        guard !sessionReadOnly, !isPreview, !credentialsDeletionInProgress else { return false }
        if maintenanceState == .ready { return uninstallRemovesData == removeData }
        guard maintenanceState == .running else { return false }
        preservePendingSubmission()
        _ = persistRecoveryJournal()
        uninstallRemovesData = removeData
        maintenanceState = .stopping
        _ = diagnosticFreshness.invalidate()
        let pending = Array(backgroundTasks.values)
        monitor?.pathUpdateHandler = nil
        monitor?.cancel(); monitor = nil
        #if os(macOS)
        for token in workspaceObservers { NSWorkspace.shared.notificationCenter.removeObserver(token) }
        workspaceObservers.removeAll()
        #endif
        pending.forEach { $0.cancel() }
        cancelOperations()
        authorizationGraceTask = nil
        authorizationGraceDeadline = nil
        environmentRefreshTask = nil
        networkInformationTask = nil
        networkInformationRefreshing = false
        invalidateTransport()
        networkRevision &+= 1
        policy.automaticEnabled = false
        currentNetworkAuthorized = false
        #if os(macOS)
        await wifiRecovery.prepareForAppMaintenance()
        #endif
        await client.resetConnections()
        for task in pending { await task.value }
        maintenanceState = .ready
        return true
    }

    private func trackedBackgroundTask(_ operation: @escaping @MainActor () async -> Void) -> Task<Void, Never> {
        let id = UUID()
        let task = Task { @MainActor [weak self] in
            defer { self?.backgroundTasks.removeValue(forKey: id) }
            await operation()
        }
        backgroundTasks[id] = task
        return task
    }

    func acknowledgeUninstallCleanup(loginItemRemoved: Bool, credentialsRemoved: Bool) {
        guard maintenanceState == .ready else { return }
        if loginItemRemoved { launchAtLogin = false }
        if credentialsRemoved {
            automaticCredentials = nil
            account = ""
            password = ""
        }
    }

    private func loadRecoveryJournal(_ url: URL) {
        recoveryJournalURL = url
        do {
            if let journal = try RecoveryJournal.read(url, now: now()) {
                savedGeneration = journal.generation
                policy.restoreRecovery(journal)
                lastRecoveryJournal = journal
                holds = policy.holds.holds
                automaticRecoveryPaused = pausedForAutomatic
            }
        } catch {
            recoveryJournalBlocked = true
            policy.suspendAuthentication(.security)
            errorMessage = SharedCopy.t("error.recoveryJournalUnreadable")
        }
    }
    private func loadEvents(from recordStoreDirectory: URL? = nil) {
        do {
            let folder = try recordStoreDirectory ?? FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                appropriateFor: nil, create: !sessionReadOnly).appendingPathComponent("SZUConnect", isDirectory: true)
            if !sessionReadOnly {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true,
                                                        attributes: [.posixPermissions: 0o700])
            }
            loadRecoveryJournal(folder.appendingPathComponent("recovery.json"))
            loadWebsitePreferences(folder.appendingPathComponent("websites.json"))
            loadNetworkMemory(folder.appendingPathComponent("network-memory.json"))
            loadNetworkLabels(folder.appendingPathComponent(NetworkLabelsStore.filename))
            healthURL = folder.appendingPathComponent("health.json")
            if let healthURL, FileManager.default.fileExists(atPath: healthURL.path) {
                do {
                    let file = try FileHandle(forReadingFrom: healthURL)
                    defer { try? file.close() }
                    let data = try file.read(upToCount: 1_048_577) ?? Data()
                    guard data.count <= 1_048_576 else { throw CocoaError(.fileReadTooLarge) }
                    let saved = try JSONDecoder().decode(ConnectionHealth.self, from: data)
                    health.restore(saved.samples)
                } catch {
                    self.healthURL = nil // Preserve the unreadable file; never overwrite it.
                    errorMessage = SharedCopy.t("error.healthUnreadable")
                }
            }
            logURL = folder.appendingPathComponent("events.json")
            if let logURL, FileManager.default.fileExists(atPath: logURL.path) {
                do {
                    events = try EventLogFile.read(from: logURL)
                } catch {
                    // Keep the unreadable original beside a fresh log instead of overwriting it.
                    if !sessionReadOnly, (try? EventLogFile.quarantine(logURL)) != nil {
                        errorMessage = SharedCopy.t("error.eventsQuarantined")
                    } else {
                        self.logURL = nil
                        errorMessage = SharedCopy.t("error.eventsUnreadable")
                    }
                }
            }
        } catch { errorMessage = SharedCopy.t("error.storage") }
    }

    private func applyDiagnosticObservation(token: UInt64, _ apply: () -> Void) {
        diagnosticApplyingObservation = true
        defer { diagnosticApplyingObservation = false; diagnosticFreshness.complete(token) }
        apply()
    }

    private func receive(_ observation: PathObservation) {
        guard !isMaintaining else { return }
        #if os(macOS)
        defer { wifiRecovery.observeRadioPower(observation.wifiRadioPowered) }
        #endif
        invalidateManualSnapshot(resumeMonitoring: true)
        defer { refreshSystemConnectivityIfNeeded() }
        observeWebsitePath(observation)
        receiveResolved(resolveAuthenticationNetwork(observation))
    }

    private func observeWebsitePath(_ observation: PathObservation) {
        guard !isMaintaining else { return }
        defer { websitePathIdentity = observation.continuity.identity }
        guard hasPath, websitePathIdentity != observation.continuity.identity else { return }
        invalidateSystemConnectivity(recheck: true)
        websiteEnvironmentRevision &+= 1
        lastInternetCheck = nil
        websiteConfigurationChanged = selectedWebsiteCount > 0
    }

    /// A persistent record describes one physical network; an operation lease describes
    /// the exact current route. Never authorize a route using a different attached record.
    private func resolveAuthenticationNetwork(_ observation: PathObservation) -> PathObservation {
        guard observation.continuity.requiresPhysicalBinding || !observation.physicalCandidates.isEmpty else { return observation }
        var unique: [CampusPhysicalRoute] = []
        for route in observation.physicalCandidates where !unique.contains(route) { unique.append(route) }
        let candidates = unique.filter { route in
            route.descriptor?.isReliable == true && unique.filter { $0.interfaceName == route.interfaceName }.count == 1
        }
        func match(_ route: CampusPhysicalRoute) -> NetworkMemory.Match {
            guard let memory, !memoryUnreadable, let descriptor = route.descriptor else { return .init() }
            return memory.match(descriptor, now: now(), exclude: suppressedMemory)
        }
        let known = candidates.filter { [.medium, .strong].contains(match($0).level) }.sorted {
            if $0.isSystemPrimary != $1.isSystemPrimary { return $0.isSystemPrimary }
            let left = match($0).level == .strong, right = match($1).level == .strong
            if left != right { return left }
            if $0.descriptor?.kind != $1.descriptor?.kind { return $0.descriptor?.kind == "ethernet" }
            return $0.interfaceName < $1.interfaceName
        }
        // Preserve an entered credential attempt and its confirmation on their proven
        // attachment. Read-only probes and discovery yield to a new preferred route.
        let preserveCredentialRoute = credentialOperation != nil && credentialOperation == policy.active
        let activeRoute = preserveCredentialRoute ? candidates.first { $0 == authenticationRoute } : nil
        let primaries = candidates.filter(\.isSystemPrimary)
        // A remembered secondary attachment does not decide the current observation
        // egress. Selecting a new primary grants neither its peer's proof nor trust.
        // Without an unambiguous primary, retain only the existing deterministic
        // remembered fallback or an independently valid sole physical candidate.
        let preferredRoute = primaries.count > 1 ? nil
            : primaries.first ?? known.first ?? (candidates.count == 1 ? candidates.first : nil)
        guard let route = activeRoute ?? preferredRoute, let descriptor = route.descriptor else {
            var continuity = observation.continuity
            continuity.authenticationRoute = nil
            continuity.requiresPhysicalBinding = true
            continuity.descriptors = []
            continuity.trustUnavailableReason = SharedCopy.t("env.identification.pending")
            return PathObservation(usable: observation.usable, interfaces: observation.interfaces,
                                   constrained: observation.constrained, continuity: continuity, physicalCandidates: observation.physicalCandidates,
                                   physicalDiagnostics: observation.physicalDiagnostics, capturedAt: observation.capturedAt)
        }
        let label = (descriptor.kind == "wifi" ? "Wi-Fi " : "有线 ") + route.interfaceName
        let prior = observation.continuity
        #if os(macOS)
        let observed = isolated ? nil : NetworkObservedLabel.capture(interface: route.interfaceName,
            kind: descriptor.kind == "wifi" ? .wifi : .ethernet, serviceID: route.serviceID)
        #else
        let observed: NetworkObservedLabel? = nil
        #endif
        let continuity = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: "physical:" + route.identity),
            linkActive: true, tunnel: prior.tunnel, physicalLabels: [label], systemPhysicalLabel: prior.systemPhysicalLabel,
            connectedPhysicalLabels: prior.connectedPhysicalLabels, sessionOnlyTrust: false, authenticationRoute: route,
            requiresPhysicalBinding: true, hasUsablePhysicalLink: true, trustUnavailableReason: nil,
            descriptors: [descriptor], label: observed, wifiLabelAttachments: prior.wifiLabelAttachments)
        // The bound physical candidate has its own live interface, source and default
        // route proof. A temporarily unavailable global TUN path cannot veto that probe.
        return PathObservation(usable: true, interfaces: label, constrained: observation.constrained,
                               continuity: continuity, physicalCandidates: observation.physicalCandidates,
                               physicalDiagnostics: observation.physicalDiagnostics, capturedAt: observation.capturedAt)
    }

    private func receiveResolved(_ observation: PathObservation) {
        guard !isMaintaining else { return }
        if physicalDiagnostics != observation.physicalDiagnostics { physicalDiagnostics = observation.physicalDiagnostics }
        physicalCollectedAt = observation.capturedAt
        if !isolated {
            let proxy = NetworkEnvironment.systemProxyState()
            diagnosticProxyKnown = proxy != nil
            systemProxyActive = proxy ?? false
        }
        receivePath(usable: observation.usable, interfaces: observation.interfaces,
                    constrained: observation.constrained, identity: observation.continuity.identity,
                    linkActive: observation.continuity.linkActive, tunnel: observation.continuity.tunnel,
                    descriptors: observation.continuity.descriptors,
                    physicalConnected: observation.continuity.hasUsablePhysicalLink || observation.continuity.linkActive,
                    systemPhysicalLabel: observation.continuity.systemPhysicalLabel,
                    connectedPhysicalLabels: observation.continuity.connectedPhysicalLabels,
                    sessionOnlyTrust: observation.continuity.sessionOnlyTrust,
                    trustUnavailableReason: observation.continuity.trustUnavailableReason,
                    authenticationRoute: observation.continuity.authenticationRoute,
                    requiresPhysicalBinding: observation.continuity.requiresPhysicalBinding)
        observeNetworkLabel(observation.continuity.label)
        observeAttachedWiFiNames(observation.continuity)
        if observation.continuity.identity == nil, let reason = observation.continuity.trustUnavailableReason {
            authorizationReason = reason
        }
    }

    private func refreshNetworkEnvironment() {
        guard !isMaintaining else { return }
        #if os(macOS)
        guard !isolated, !sleeping, let path = monitor?.currentPath else { return }
        let token = diagnosticFreshness.invalidate()
        let observation = PathObservation.capture(path)
        applyDiagnosticObservation(token: token) { applyEnvironmentObservation(observation) }
        #endif
    }

    private func refreshNetworkEnvironmentInBackground() async {
        guard !isMaintaining else { return }
        #if os(macOS)
        guard !isolated, !sleeping, let path = monitor?.currentPath else { return }
        let revision = networkRevision
        let websiteRevision = websiteEnvironmentRevision
        let token = diagnosticFreshness.invalidate()
        let observation = await Task.detached(priority: .utility) { PathObservation.capture(path) }.value
        guard !isMaintaining, !Task.isCancelled, !sleeping, revision == networkRevision,
              websiteRevision == websiteEnvironmentRevision else { return }
        applyDiagnosticObservation(token: token) { applyEnvironmentObservation(observation) }
        #endif
    }

    func applyEnvironmentObservation(_ incoming: PathObservation) {
        guard !isMaintaining else { return }
        #if os(macOS)
        defer { wifiRecovery.observeRadioPower(incoming.wifiRadioPowered) }
        #endif
        defer { if isolated && !diagnosticApplyingObservation { diagnosticFreshness.completeCurrent() } }
        physicalCollectedAt = incoming.capturedAt
        invalidateManualSnapshot(resumeMonitoring: true)
        // Identical local snapshots must not invalidate every model-observing view on each poll.
        if physicalDiagnostics != incoming.physicalDiagnostics { physicalDiagnostics = incoming.physicalDiagnostics }
        defer { refreshSystemConnectivityIfNeeded() }
        observeWebsitePath(incoming)
        let observation = resolveAuthenticationNetwork(incoming)
        observeAttachedWiFiNames(observation.continuity)
        let proxyValue = isolated ? systemProxyActive : NetworkEnvironment.systemProxyState()
        diagnosticProxyKnown = proxyValue != nil
        let proxy = proxyValue ?? false
        if proxy != systemProxyActive {
            invalidateSystemConnectivity(recheck: true)
            systemProxyActive = proxy
            websiteEnvironmentRevision &+= 1
            lastInternetCheck = nil
            websiteConfigurationChanged = selectedWebsiteCount > 0
        }
        // Names are presentation metadata; a new permission or service label never changes trust.
        // Only bind after receive has established the matching current path below.
        if observation.continuity.identity == currentIdentity,
           descriptorsMatch(observation.continuity.descriptors) {
            observeNetworkLabel(observation.continuity.label)
        }
        // Avoid publishing identical snapshots or cancelling work on a polling tick.
        guard !hasPath || observation.continuity.identity != currentIdentity
            || observation.usable != policy.available || observation.continuity.linkActive != lastLinkActive
            || (observation.continuity.hasUsablePhysicalLink || observation.continuity.linkActive) != hasPhysicalConnection
            || observation.continuity.tunnel != tunnelActive || observation.constrained != constrained
            || observation.continuity.systemPhysicalLabel != systemPhysicalLabel
            || observation.continuity.connectedPhysicalLabels != connectedPhysicalLabels
            || observation.continuity.sessionOnlyTrust != sessionOnlyTrust
            || observation.continuity.trustUnavailableReason != networkTrustUnavailableReason
            || observation.continuity.authenticationRoute != authenticationRoute
            || observation.continuity.requiresPhysicalBinding != requiresPhysicalBinding
            || !descriptorsMatch(observation.continuity.descriptors) || observation.interfaces != lastPathInterfaces
            else { return }
        receiveResolved(observation)
    }

    private func descriptorsMatch(_ descriptors: [NetworkDescriptor]) -> Bool {
        guard currentDescriptors.count == descriptors.count else { return false }
        var remaining = descriptors
        for descriptor in currentDescriptors {
            guard let index = remaining.firstIndex(of: descriptor) else { return false }
            remaining.remove(at: index)
        }
        return true
    }

    private func invalidateTransport() { transportGeneration &+= 1 }

    private func prepareTransport(for operation: ConnectionPolicy.Operation) async -> Bool {
        let generation = transportGeneration
        let expected = authenticationRoute
        if preparedTransportGeneration != generation {
            await client.resetConnections()
            guard !Task.isCancelled, policy.owns(operation), generation == transportGeneration else { return false }
            preparedTransportGeneration = generation
        }
        guard await client.setExpectedPhysicalRoute(expected), authenticationRoute == expected,
              generation == transportGeneration else { return false }
        return !Task.isCancelled && policy.owns(operation)
    }

    /// Repeated notifications for the same attachment are observations, not network changes.
    /// Identity is RAM-only; an absent fingerprint can never match another absent fingerprint.
    func receivePath(usable: Bool, interfaces: String, constrained: Bool = false,
                     identity: NetworkIdentity? = nil, linkActive: Bool = true, now: Date = Date(),
                     tunnel: Bool = false, descriptors: [NetworkDescriptor] = [], physicalConnected: Bool? = nil,
                     systemPhysicalLabel: String? = nil, connectedPhysicalLabels: [String] = [],
                     sessionOnlyTrust: Bool = false, trustUnavailableReason: String? = nil,
                     authenticationRoute: CampusPhysicalRoute? = nil, requiresPhysicalBinding: Bool = false) {
        guard !isMaintaining else { return }
        defer {
            if isolated && !diagnosticApplyingObservation { diagnosticFreshness.completeCurrent() }
            if isolated { diagnosticProxyKnown = true }
        }
        invalidateManualSnapshot(resumeMonitoring: true)
        defer { refreshSystemConnectivityIfNeeded() }
        let trustScopeChanged = hasPath && sessionOnlyTrust != self.sessionOnlyTrust
        let bindingChanged = hasPath && (authenticationRoute != self.authenticationRoute || requiresPhysicalBinding != self.requiresPhysicalBinding)
        self.authenticationRoute = authenticationRoute
        self.requiresPhysicalBinding = requiresPhysicalBinding
        hasPhysicalConnection = physicalConnected ?? (usable || linkActive)
        self.systemPhysicalLabel = systemPhysicalLabel
        self.connectedPhysicalLabels = connectedPhysicalLabels
        self.sessionOnlyTrust = sessionOnlyTrust
        self.networkTrustUnavailableReason = trustUnavailableReason
        observedNetworkLabel = nil
        let firstPath = !hasPath
        let sameIdentity = identity != nil && identity == currentIdentity
        let previouslyAvailable = policy.available
        let tunnelChanged = hasPath && tunnel != tunnelActive
        let authenticationTunnelChanged = tunnelChanged && !(requiresPhysicalBinding && authenticationRoute != nil && !bindingChanged)
        let descriptorsChanged = hasPath && !descriptorsMatch(descriptors)
        let sameUnknownObservation = hasPath && identity == nil && currentIdentity == nil
            && interfaces == lastPathInterfaces && !descriptorsChanged && !tunnelChanged && !trustScopeChanged && !bindingChanged
            && linkActive == lastLinkActive
        hasPath = true
        lastPathInterfaces = interfaces
        self.constrained = constrained
        lastLinkActive = linkActive
        tunnelActive = tunnel
        if usable, previouslyAvailable, !sleeping, !authenticationTunnelChanged, !descriptorsChanged, !trustScopeChanged, !bindingChanged,
           (sameIdentity && linkActive) || sameUnknownObservation {
            if tunnelChanged {
                resetCampusDirectFailures()
                invalidateSystemConnectivity(recheck: true)
                refreshSystemConnectivityIfNeeded()
                websiteEnvironmentRevision &+= 1
                lastInternetCheck = nil
                websiteConfigurationChanged = selectedWebsiteCount > 0
            }
            interfaceSummary = interfaces
            refreshAuthorization()
            updateWiFiRecoveryContext()
            return // Repeated observations preserve ownership, never establish network trust.
        }
        let recheckSystem = systemConnectivityChecking || systemConnectivity != nil || systemConnectivityRecheckNeeded
        let interruptedMemoryVerification = isVerifyingMemory
        let interruptedServiceOnly = campusServiceOnlyVerification
        cancelOperations()
        if interruptedMemoryVerification {
            let prefix = interruptedServiceOnly ? "campus.verify." : "memory.verification."
            setVerificationFeedback(prefix + (usable ? "changed" : "disconnected"))
        }
        systemConnectivityRecheckNeeded = recheckSystem && usable && !sleeping && monitoringMode != .manual
        if !firstPath { invalidateTransport() }
        networkRevision &+= 1
        // Even a brief disconnect with the same identity needs a new service
        // observation. A remembered attachment is not evidence of this connection.
        lastDiscovery = nil
        if !interruptedMemoryVerification, verificationFeedback?.serviceOnly == true { setVerificationFeedback(nil) }
        lastInternetCheck = nil
        websiteConfigurationChanged = false
        if !sameIdentity || authenticationTunnelChanged || descriptorsChanged || trustScopeChanged || bindingChanged {
            if !interruptedMemoryVerification { setVerificationFeedback(nil) }
            // A different network: holds scoped to the previous network no longer apply.
            networkEpoch &+= 1
            partialRecovery.reset()
            policy.pathChanged(to: networkEpoch)
        }
        policy.available = usable && !sleeping
        interfaceSummary = usable && !interfaces.isEmpty ? interfaces : SharedCopy.t("env.physicalUnavailable")

        if !usable, sameIdentity, linkActive, !sleeping, !descriptorsChanged, !authenticationTunnelChanged, !trustScopeChanged, !bindingChanged,
           identity?.permitsBriefInterruption == true, policy.currentNetworkAuthorized {
            if authorizationGraceDeadline == nil {
                authorizationGraceDeadline = now.addingTimeInterval(15)
                scheduleAuthorizationGraceExpiry()
            }
            if let deadline = authorizationGraceDeadline, now > deadline {
                revokeAuthorization(reason: SharedCopy.t("guard.connectionTrustExpired"))
            } else {
                authorizationReason = SharedCopy.t("guard.waitingRecovery")
            }
        } else if !sameIdentity || authenticationTunnelChanged || descriptorsChanged || trustScopeChanged || bindingChanged || !linkActive || sleeping || !usable {
            let reason: String
            if firstPath { reason = defaultTrustReason }
            else if !linkActive { reason = SharedCopy.t("guard.disconnected") }
            else if identity == nil { reason = SharedCopy.t("guard.networkUnknown") }
            else if !usable { reason = SharedCopy.t("guard.networkUnavailable") }
            else if tunnelChanged { reason = SharedCopy.t("guard.tunnelChanged") }
            else { reason = SharedCopy.t("guard.networkChanged") }
            revokeAuthorization(reason: reason)
            policy.resetFailures()
        } else if let deadline = authorizationGraceDeadline {
            if now > deadline { revokeAuthorization(reason: SharedCopy.f("guard.trustTimeout", ["seconds": "15"])) }
            clearAuthorizationGrace()
        }
        currentIdentity = identity
        currentDescriptors = descriptors
        if usable, !firstPath || (authenticationRoute != nil && networkRemembered) {
            rapidRecoveryDeadline = self.now().addingTimeInterval(45)
        }
        lastCheckedAt = nil
        isOnline = false
        lastDiagnosis = nil
        lastProbeMode = nil
        lastProbePassedCount = 0
        probeSummary = firstPath ? SharedCopy.t("status.idle.headline") : SharedCopy.t("internet.pending.title")
        authenticationSummary = SharedCopy.t("guard.checkFirst")
        refreshAuthorization()
        if sleeping { refreshRecovery(); return }
        if usable {
            status = SharedCopy.t("tray.state.idle")
            detail = policy.automaticEnabled ? SharedCopy.t("guard.readyAutomatic") : SharedCopy.t("mode.manual.detail")
            startLoop(initialDelay: rapidRecoveryDeadline != nil ? 0 : 2)
        } else {
            status = SharedCopy.t("env.access.none")
            detail = SharedCopy.t("status.noNetwork.detail")
            if previouslyAvailable { record(SharedCopy.t("event.pathLost")) }
        }
        refreshRecovery()
    }

    private func clearAuthorizationGrace() {
        authorizationGraceTask?.cancel()
        authorizationGraceTask = nil
        authorizationGraceDeadline = nil
    }
    private func scheduleAuthorizationGraceExpiry() {
        guard !isMaintaining else { return }
        guard !isolated else { return }
        authorizationGraceTask?.cancel()
        authorizationGraceTask = trackedBackgroundTask { [weak self] in
            do { try await Task.sleep(for: .seconds(15)) } catch { return }
            guard let self, !self.policy.available, self.authorizationGraceDeadline != nil else { return }
            self.revokeAuthorization(reason: SharedCopy.t("guard.connectionTrustExpired"))
        }
    }
    private func revokeAuthorization(reason: String) {
        let previouslyAuthorized = policy.currentNetworkAuthorized
        policy.revokeAuthorization()
        clearAuthorizationGrace()
        authorizationReason = reason
        refreshAuthorization()
        if previouslyAuthorized { record(reason) }
    }
    private func refreshAuthorization() {
        guard !isMaintaining else { return }
        currentNetworkAuthorized = policy.currentNetworkAuthorized
        if currentNetworkAuthorized {
            authorizationStatusText = policy.available ? SharedCopy.t("guard.trustGranted") : SharedCopy.t("guard.trustWaitingRecovery")
            if policy.available {
                authorizationReason = SharedCopy.t("memory.sessionTrusted")
            }
        } else {
            authorizationStatusText = SharedCopy.t("guard.trustNotGranted")
        }
    }

    private func configurationChanged() {
        guard !isMaintaining else { return }
        guard initialized, !credentialsDeletionInProgress else { return }
        configurationRevision &+= 1
        let interrupted = busy
        cancelOperations()
        policy.resetFailures()
        if interrupted {
            // The stopped round must not leave a stale "正在登录" behind.
            status = isOnline ? SharedCopy.t("diagnosis.online.title") : SharedCopy.t("status.cancelled.headline")
            detail = SharedCopy.t("guard.credentialsEdited")
        }
        // Editing an account does not change which physical network the user authorized.
        refreshRecovery()
        startLoop()
    }

    // MARK: - Account draft

    var hasSavedCredentials: Bool { automaticCredentials != nil }
    var hasCompleteDraft: Bool {
        !account.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !password.isEmpty
    }
    var hasUnsavedCredentialChanges: Bool {
        let draftAccount = account.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let saved = automaticCredentials else { return !draftAccount.isEmpty || !password.isEmpty }
        return draftAccount != saved.account || password != saved.password
    }
    /// Never shows the full account; enough to recognise which account is saved.
    var savedAccountHint: String? { automaticCredentials.map { Self.maskedAccount($0.account) } }

    nonisolated static func maskedAccount(_ account: String) -> String {
        let characters = Array(account)
        guard characters.count > 4 else { return String(characters.prefix(1)) + "•••" }
        let head = String(characters.prefix(characters.count >= 8 ? 4 : 2))
        return head + "••••" + String(characters.suffix(2))
    }

    func saveCredentials() {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, !credentialsDeletionInProgress else { return }
        let credentials = Credentials(account: account.trimmingCharacters(in: .whitespacesAndNewlines), password: password)
        guard !credentials.account.isEmpty, !credentials.password.isEmpty,
              credentials.account.utf8.count <= 256, credentials.password.utf8.count <= 1024 else {
            errorMessage = SharedCopy.t("account.invalid"); return
        }
        do {
            if !isolated { try CredentialStore.save(credentials) }
            if automaticCredentials?.account != credentials.account || automaticCredentials?.password != credentials.password {
                // A different saved account: holds about the previous one no longer apply.
                savedGeneration &+= 1
                partialRecovery.reset()
                policy.credentialsChanged(to: savedGeneration)
            }
            automaticCredentials = credentials
            // A save is explicit, but retry is a separate user decision after rejection.
            record(SharedCopy.f("event.saved", ["store": SharedCopy.t("app.store")]))
            refreshRecovery()
        } catch { errorMessage = SharedCopy.t("account.saveFailed") }
    }
    /// Restores the saved account into the draft. Never touches the Keychain.
    func revertCredentialDraft() {
        guard !isMaintaining else { return }
        account = automaticCredentials?.account ?? ""
        password = automaticCredentials?.password ?? ""
    }

    /// The caller presents confirmation. The exact application Keychain item is the only deletion target.
    func deleteSavedCredentials() async {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, !credentialsDeletionInProgress else { return }
        credentialsDeletionInProgress = true
        credentialsDeletionMessage = nil
        credentialsDeletionFailed = false
        // Revoke task ownership before a Keychain prompt or asynchronous deletion can yield.
        let interrupted = busy
        cancelOperations()
        if interrupted {
            engineStatus = isOnline ? "online" : "cancelled"
            status = SharedCopy.t(isOnline ? "diagnosis.online.title" : "status.cancelled.headline")
            detail = SharedCopy.t("status.cancelled.detail")
        }
        refreshRecovery()
        defer { credentialsDeletionInProgress = false; refreshRecovery(); startLoop() }
        do {
            try await credentialDeletion()
            automaticCredentials = nil
            account = ""
            password = ""
            credentialsDeletionMessage = SharedCopy.t("account.deleted")
            authenticationSummary = SharedCopy.t("guard.missingCredentials")
            record(SharedCopy.t("account.deleted"))
        } catch {
            policy.pauseByUser()
            credentialsDeletionFailed = true
            credentialsDeletionMessage = SharedCopy.t("account.deleteFailed")
        }
    }

    // MARK: - User actions

    var networkAvailable: Bool { policy.available }
    var systemPathAvailable: Bool { networkAvailable || hasPhysicalConnection }
    /// Display evidence never hides an active login or any authentication protection.
    var systemPathCanLead: Bool {
        guard !busy, !sleeping, !pausedForAutomatic, holds.isEmpty,
              (cooldownRemaining ?? 0) <= 0, systemPathAvailable else { return false }
        return ["idle", "offline", "online", "noNetwork", "unconfirmed", "needsTrust", "needsAccount"].contains(engineStatus)
    }
    /// Manual and automatic submissions are possible in this session and guard mode.
    var canAuthenticate: Bool { !isMaintaining && !sessionReadOnly && !credentialsDeletionInProgress && monitoringMode != .observe }
    /// Dormitory (or automatic) recovery needs an explicit trust decision for this network.
    var needsNetworkTrust: Bool { monitoringMode == .recover && area != .teaching }
    /// Work the user should see: any manual action, or anything while not yet online.
    /// Routine background checks of a healthy connection stay quiet.
    var isForegroundBusy: Bool { busy && (manual != nil || manualSnapshotID != nil || !isOnline || partialRecoveryInProgress) }

    func connect() {
        guard !isMaintaining else { return }
        guard canAuthenticate else { checkConnection(); return }
        // Probe before every manual submission to avoid logging in an already-online session.
        startManual(mayAuthenticate: true)
    }
    /// The probe already passed, yet the user still has trouble (for example an IPv4 session
    /// that expired on a dual-stack campus). One explicit, manual submission; never automatic.
    var canOfferPartialLogin: Bool {
        canAuthenticate && hasCompleteDraft && lastInternetCheck?.hasRecoveryEvidence == true
            && !websiteConfigurationChanged
    }
    var canOfferForcedLogin: Bool {
        canAuthenticate && hasCompleteDraft && !websiteConfigurationChanged
            && lastInternetCheck?.base.online == true && lastInternetCheck?.verdict != .cancelled
            && lastInternetCheck?.hasRecoveryEvidence == false
            && (offerForcedLogin || lastInternetCheck?.verdict == .partial)
    }
    /// Captured before presenting the warning, never after the user accepts it.
    struct ForcedLoginConfirmation: Identifiable, Equatable {
        let id: UUID
        fileprivate let epoch: UInt64
        fileprivate let revision: UInt
        fileprivate let route: CampusPhysicalRoute?
        fileprivate let configuration: UInt64
        fileprivate let action: UInt64
        fileprivate let result: UInt64
        fileprivate let websites: UInt64
        fileprivate let credentials: UInt64
        fileprivate let memory: UInt64
        fileprivate let mode: MonitoringMode
        fileprivate let area: CampusArea
        fileprivate let probe: ConnectivityProbeMode
    }
    func forcedLoginConfirmation() -> ForcedLoginConfirmation? {
        guard canOfferForcedLogin, !busy, !sleeping else { return nil }
        return ForcedLoginConfirmation(id: UUID(), epoch: networkEpoch, revision: networkRevision,
            route: authenticationRoute, configuration: configurationRevision, action: manualActionRevision, result: connectivityResultRevision,
            websites: websiteEnvironmentRevision, credentials: savedGeneration, memory: memoryGeneration,
            mode: monitoringMode, area: area, probe: probeMode)
    }
    func canContinueForcedLogin(_ confirmation: ForcedLoginConfirmation) -> Bool {
        canOfferForcedLogin && !busy && forcedLoginContextMatches(confirmation)
    }
    private func forcedLoginContextMatches(_ value: ForcedLoginConfirmation) -> Bool {
        !isMaintaining && !sleeping && value.epoch == networkEpoch && value.revision == networkRevision
            && value.route == authenticationRoute && value.configuration == configurationRevision
            && value.action == manualActionRevision
            && value.result == connectivityResultRevision && value.websites == websiteEnvironmentRevision
            && value.credentials == savedGeneration && value.memory == memoryGeneration
            && value.mode == monitoringMode && value.area == area && value.probe == probeMode
    }
    func connectEvenIfOnline(confirmation: ForcedLoginConfirmation?) {
        guard let confirmation, canContinueForcedLogin(confirmation) else { return }
        startManual(mayAuthenticate: true, confirmation: confirmation)
    }
    func checkConnection() { startManual(mayAuthenticate: false) }

    /// No result here changes trust, holds, authentication, history or recovery policy.
    func refreshSystemConnectivity() {
        guard !isMaintaining else { return }
        guard let probe = systemConnectivityProbe, !sleeping, policy.foreground,
              networkAvailable || hasPhysicalConnection else { return }
        systemConnectivityTask?.cancel()
        systemConnectivityGeneration &+= 1
        let generation = systemConnectivityGeneration
        let mode = probeMode
        systemConnectivity = nil
        systemConnectivityCheckedAt = nil
        systemConnectivityChecking = true
        systemConnectivityRecheckNeeded = false
        systemConnectivityTask = trackedBackgroundTask { [weak self] in
            let report = await probe(mode)
            guard let self, !self.isMaintaining, !Task.isCancelled, generation == self.systemConnectivityGeneration,
                  !self.sleeping, self.networkAvailable || self.hasPhysicalConnection else { return }
            self.systemConnectivity = report
            self.systemConnectivityCheckedAt = self.now()
            self.systemConnectivityChecking = false
            self.systemConnectivityTask = nil
            self.observeDirectNetworkResult()
        }
    }

    func refreshDisplayedNetwork() {
        guard !isMaintaining else { return }
        guard !networkInformationRefreshing, !sleeping else { return }
        manualActionRevision &+= 1
        invalidateManualSnapshot(resumeMonitoring: true)
        networkInformationRefreshing = true
        networkInformationFeedback = nil
        let token = diagnosticFreshness.invalidate()
        let revision = networkRevision
        networkInformationTask = trackedBackgroundTask { [weak self] in
            guard let self else { return }
            defer { self.networkInformationRefreshing = false; self.networkInformationTask = nil }
            do {
                let observation: PathObservation?
                if let reader = self.networkInformationReader { observation = try await reader() }
                else if self.isolated { observation = nil }
                else if let path = self.monitor?.currentPath {
                    observation = await Task.detached(priority: .utility) { PathObservation.capture(path) }.value
                } else { throw CancellationError() }
                guard !self.isMaintaining, !Task.isCancelled, !self.sleeping else { return }
                guard self.networkRevision == revision else {
                    self.networkInformationFeedback = SharedCopy.t("env.refreshChanged"); return
                }
                if let observation { self.applyDiagnosticObservation(token: token) { self.applyEnvironmentObservation(observation) } }
                else { self.diagnosticFreshness.complete(token) }
                let partial = self.hasPhysicalConnection && self.currentIdentity == nil
                self.networkInformationFeedback = SharedCopy.f(partial ? "env.refreshedPartial" : "env.refreshed",
                    ["time": self.now().formatted(date: .omitted, time: .standard)])
            } catch {
                if !Task.isCancelled { self.networkInformationFeedback = SharedCopy.t("env.refreshFailed") }
            }
        }
    }

    /// Capture anonymous local evidence only. In particular, do not apply a new path here:
    /// receivePath can schedule automatic work, while this action must never start a probe or login.
    /// Normal path monitoring independently refreshes the live authentication path.
    func refreshPhysicalIdentification() {
        guard !isMaintaining, !networkInformationRefreshing, !sleeping else { return }
        manualActionRevision &+= 1
        invalidateManualSnapshot(resumeMonitoring: true)
        networkInformationRefreshing = true
        networkInformationFeedback = nil
        let revision = networkRevision
        networkInformationTask = trackedBackgroundTask { [weak self] in
            guard let self else { return }
            defer { self.networkInformationRefreshing = false; self.networkInformationTask = nil }
            do {
                let observation: PathObservation
                if let reader = self.networkInformationReader { observation = try await reader() }
                else if !self.isolated, let path = self.monitor?.currentPath {
                    observation = await Task.detached(priority: .utility) { PathObservation.capture(path) }.value
                } else { throw CancellationError() }
                guard !self.isMaintaining, !Task.isCancelled, !self.sleeping else { return }
                guard self.networkRevision == revision else {
                    self.networkInformationFeedback = SharedCopy.t("env.refreshChanged"); return
                }
                if self.physicalDiagnostics != observation.physicalDiagnostics { self.physicalDiagnostics = observation.physicalDiagnostics }
                self.physicalCollectedAt = observation.capturedAt
                self.networkInformationFeedback = SharedCopy.t("env.identification.refreshed")
            } catch {
                if !Task.isCancelled { self.networkInformationFeedback = SharedCopy.t("env.identification.readFailed") }
            }
        }
    }

    /// Only fixed enums and bounded counts may cross the diagnostic-export boundary.
    private var anonymousPhysicalDiagnostics: CampusPhysicalNetwork.CandidateDiagnostics? {
        guard let physicalDiagnostics else { return nil }
        var safe = CampusPhysicalNetwork.CandidateDiagnostics()
        safe.physicalInterfaceCount = max(0, min(4096, physicalDiagnostics.physicalInterfaceCount))
        safe.serviceCount = max(0, min(4096, physicalDiagnostics.serviceCount))
        safe.candidateCount = max(0, min(4096, physicalDiagnostics.candidateCount))
        for reason in CampusPhysicalNetwork.CandidateRejection.allCases {
            if let count = physicalDiagnostics.rejections[reason.rawValue], count > 0 {
                safe.rejections[reason.rawValue] = min(4096, count)
            }
        }
        return safe
    }

    var physicalIdentificationDetail: String {
        guard let diagnostics = anonymousPhysicalDiagnostics else {
            return SharedCopy.t(physicalNetworkIdentified ? "env.identification.ready" : "env.identification.notCollected")
        }
        // A locally eligible candidate is not yet the model's selected/authorized path.
        if diagnostics.candidateCount > 0 {
            return SharedCopy.t(physicalNetworkIdentified ? "env.identification.ready" : "env.identification.awaitingRefresh")
        }
        let priority: [CampusPhysicalNetwork.CandidateRejection] = [
            .systemConfigurationUnavailable, .interfaceSnapshotUnavailable, .routeSnapshotUnavailable,
            .addressSnapshotUnavailable, .serviceSnapshotUnavailable, .invalidDHCPServer, .invalidDHCPDNS,
            .invalidServiceRecord, .duplicateService, .missingDNS, .invalidDNS, .missingDefaultRoute,
            .conflictingDefaultRoutes, .gatewayMismatch, .missingSourceAddress, .invalidSourceAddress,
            .sourceAddressMismatch, .invalidSubnet, .invalidGateway, .missingInterfaceIndex,
            .missingDescriptor, .unstableSnapshot, .noIPv4Service, .inactiveLink, .unsupportedInterface,
            .unsupportedPlatform
        ]
        if let reason = priority.first(where: {
            (diagnostics.rejections[$0.rawValue] ?? 0) > 0
        }) {
            return SharedCopy.t("env.identification.reason." + reason.rawValue)
        }
        return SharedCopy.t("env.identification.readFailed")
    }

    private func invalidateSystemConnectivity(recheck: Bool = false) {
        let hadRequest = systemConnectivityChecking || systemConnectivity != nil || systemConnectivityRecheckNeeded
        systemConnectivityTask?.cancel()
        systemConnectivityTask = nil
        systemConnectivityGeneration &+= 1
        systemConnectivity = nil
        systemConnectivityCheckedAt = nil
        systemConnectivityChecking = false
        systemConnectivityRecheckNeeded = recheck && hadRequest
    }

    private func refreshSystemConnectivityIfNeeded() {
        if systemConnectivityRecheckNeeded { refreshSystemConnectivity() }
    }
    private func loadWebsitePreferences(_ url: URL) {
        websitePreferencesURL = url
        do {
            let saved = try WebsitePreferences.read(url)
            customWebsiteTargets = saved.targets; customWebsiteStrategy = saved.strategy
        }
        catch {
            websitePreferencesCorrupt = true
            websiteConfigurationError = SharedCopy.t("internet.settingsUnreadable")
        }
    }
    var selectedWebsiteCount: Int { customWebsiteTargets.filter(\.enabled).count }
    var canEditWebsiteTargets: Bool { !isMaintaining && !sessionReadOnly && (!busy || probeOwner != nil) }

    @discardableResult
    func saveCustomWebsites(_ targets: [WebsiteTarget], preferComplete: Bool = true, strategy: WebsiteProbeStrategy? = nil) -> Bool {
        guard !isMaintaining else { return false }
        guard !sessionReadOnly else { return false }
        let selectedStrategy = strategy ?? customWebsiteStrategy
        guard canEditWebsiteTargets else {
            websiteConfigurationError = SharedCopy.t("internet.settingsBusy"); return false
        }
        let validated: [WebsiteTarget]
        do { validated = try WebsiteTarget.validated(targets) }
        catch { websiteConfigurationError = (error as? LocalizedError)?.errorDescription ?? SharedCopy.t("internet.settingsInvalid"); return false }
        do {
            if !isolated || websitePreferencesURL != nil {
                guard let url = websitePreferencesURL else { throw CocoaError(.fileWriteUnknown) }
                if websitePreferencesCorrupt { try WebsitePreferences.quarantine(url) }
                try WebsitePreferences(targets: validated, strategy: selectedStrategy).write(url)
                websitePreferencesCorrupt = false
            }
        } catch { websiteConfigurationError = SharedCopy.t("internet.settingsWriteFailed"); return false }
        websiteConfigurationError = nil
        guard validated != customWebsiteTargets || selectedStrategy != customWebsiteStrategy else { return true }
        let detectionChanged = validated.filter(\.enabled) != customWebsiteTargets.filter(\.enabled)
            || selectedStrategy != customWebsiteStrategy
        if !detectionChanged {
            // Stored disabled rows are editing state, not an in-flight request input.
            // Their successful save must not cancel a round or advance its next plan.
            customWebsiteTargets = validated
            return true
        }
        cancelOperations()
        customWebsiteTargets = validated
        customWebsiteStrategy = selectedStrategy
        lastInternetCheck = nil
        websiteConfigurationChanged = true
        if preferComplete && validated.contains(where: \.enabled) { setProbeMode(.complete) }
        if isOnline { engineStatus = "online" } else { engineStatus = "idle" }
        startLoop(initialDelay: 2)
        return true
    }
    func setWebsiteEnabled(_ id: String, enabled: Bool) {
        guard !isMaintaining else { return }
        var targets = customWebsiteTargets
        guard let index = targets.firstIndex(where: { $0.id == id }) else { return }
        targets[index].enabled = enabled
        _ = saveCustomWebsites(targets, preferComplete: enabled)
    }
    func setWebsiteStrategy(_ strategy: WebsiteProbeStrategy) {
        guard !isMaintaining else { return }
        _ = saveCustomWebsites(customWebsiteTargets, preferComplete: false, strategy: strategy)
    }
    func removeWebsite(_ id: String) {
        guard !isMaintaining else { return }
        _ = saveCustomWebsites(customWebsiteTargets.filter { $0.id != id }, preferComplete: false) }
    @discardableResult func addWebsite(_ input: String) -> Bool {
        saveCustomWebsites(customWebsiteTargets + [WebsiteTarget(url: input)], preferComplete: true)
    }
    private func websitePresetURL(key: String) -> String? {
        guard let preset = SharedLinks.websitePresets.first(where: { $0.key == key }) else { return nil }
        return try? WebsiteTarget.validated([WebsiteTarget(id: "preset-lookup", url: preset.url, enabled: false)]).first?.url
    }
    func hasWebsitePreset(key: String) -> Bool {
        guard let url = websitePresetURL(key: key) else { return false }
        return customWebsiteTargets.contains { $0.url == url }
    }
    func canAddWebsitePreset(key: String) -> Bool {
        !busy && canEditWebsiteTargets && customWebsiteTargets.count < WebsiteTarget.maximumCount
            && websitePresetURL(key: key) != nil && !hasWebsitePreset(key: key)
    }
    @discardableResult func addWebsitePreset(key: String) -> Bool {
        guard !isMaintaining, !sessionReadOnly else { return false }
        guard !busy, canEditWebsiteTargets else {
            websiteConfigurationError = SharedCopy.t("internet.settingsBusy"); return false
        }
        guard let url = websitePresetURL(key: key) else {
            websiteConfigurationError = SharedCopy.t("internet.settingsInvalid"); return false
        }
        guard !hasWebsitePreset(key: key) else {
            websiteConfigurationError = SharedCopy.t("internet.duplicateWebsite"); return false
        }
        return saveCustomWebsites(customWebsiteTargets + [WebsiteTarget(url: url, enabled: false)], preferComplete: false)
    }
    private func runInternetCheck(mode: ConnectivityProbeMode, targets: [WebsiteTarget], strategy: WebsiteProbeStrategy,
                                  prioritizeCampusRecovery: Bool = false, deferWebsites: Bool = false,
                                  diagnosticContext: DiagnosticSampleContext? = nil, diagnosticInterface: String = "") async -> InternetCheckReport {
        let check = UUID()
        activeInternetCheck = check
        defer { if activeInternetCheck == check { activeInternetCheck = nil } }
        let websiteRevision = websiteEnvironmentRevision
        let report = await InternetChecker.check(mode: mode, targets: targets, checker: websiteChecker, strategy: strategy,
            prioritizeCampusRecovery: prioritizeCampusRecovery, deferWebsites: deferWebsites) { [client] selectedMode in
            await client.probe(mode: selectedMode)
        }
        if let internetCheckDelivery { await internetCheckDelivery(report) }
        appendHistoricalReport(report, context: diagnosticContext, interface: diagnosticInterface)
        guard websiteRevision == websiteEnvironmentRevision else {
            // TUN/proxy changes obsolete website observations, but not the bound campus probe.
            return InternetCheckReport(base: report.base, websites: [], selectedCount: report.selectedCount,
                wasCancelled: report.wasCancelled, websitesDeferred: true, completedAt: report.completedAt)
        }
        return report
    }

    func setProbeMode(_ mode: ConnectivityProbeMode) {
        guard !isMaintaining else { return }
        guard probeMode != mode else { return }
        manualActionRevision &+= 1
        invalidateManualSnapshot(resumeMonitoring: true)
        if mode == .quick { recoveryFullCheckPending = false }
        sessionAuditDeadline = nil
        partialRecovery.resetConfirmation()
        probeMode = mode
        defaults?.set(mode.rawValue, forKey: "probeMode")
        // A local display preference never cancels or starts authentication.
    }
    // MARK: - Network memory

    /// Best match of the stored records for the live physical attachment(s). Computed on demand from RAM state and
    /// the stored keyed hashes, so sleep, availability and revocation take effect immediately.
    var memoryMatch: NetworkMemory.Match {
        guard !requiresPhysicalBinding || authenticationRoute != nil else { return .init() }
        guard !sessionOnlyTrust, let memory, !memoryUnreadable, policy.available, !sleeping, currentIdentity != nil, lastLinkActive, !currentDescriptors.isEmpty else { return NetworkMemory.Match() }
        return memory.matchAny(currentDescriptors, now: now(), exclude: suppressedMemory)
    }
    var memoryLevel: MatchLevel { memoryMatch.level }
    /// Only records established with both campus proofs may authorize a later dormitory submission.
    var dormitoryMemoryLevel: MatchLevel {
        guard !requiresPhysicalBinding || authenticationRoute != nil else { return .none }
        guard !sessionOnlyTrust, let memory, !memoryUnreadable, policy.available, !sleeping, currentIdentity != nil,
              lastLinkActive, !currentDescriptors.isEmpty else { return .none }
        return memory.matchAny(currentDescriptors, now: now(), exclude: suppressedMemory,
                               requireDormitoryProof: true).level
    }
    /// A tunnel or several live interfaces make the physical path ambiguous: both campus proofs are then required.
    var memoryAmbiguous: Bool { tunnelActive || hasMultipleNetworkAttachments }
    var physicalNetworkIdentified: Bool { currentIdentity != nil && lastLinkActive && (!requiresPhysicalBinding || authenticationRoute != nil) }
    var physicalIdentificationNeeded: Bool {
        !physicalNetworkIdentified && (networkAvailable || hasPhysicalConnection || physicalDiagnostics != nil)
    }
    var authenticationTargetName: String? {
        guard let route = authenticationRoute else { return nil }
        if networkRemembered { return networkLabels.displayName(for: memoryMatch.record) }
        return observedNetworkLabel?.displayName ?? (route.descriptor?.kind == "wifi" ? SharedCopy.t("memory.item.wifi") : SharedCopy.t("memory.item.ethernet"))
    }
    var trustUnavailableDetail: String {
        networkTrustUnavailableReason ?? SharedCopy.t("banner.networkUnidentified.detail")
    }
    var hasMultipleNetworkAttachments: Bool { connectedPhysicalLabels.count > 1 || currentDescriptors.count > 1 }
    var usingWiredCampusRoute: Bool { authenticationRoute?.descriptor?.kind == "ethernet" }
    var networkRemembered: Bool {
        let level = memoryLevel
        return level == .medium || level == .strong
    }
    var rememberedCampusArea: CampusArea? {
        guard area == .automatic, authenticationRoute != nil,
              currentDescriptors.count == 1, currentDescriptors[0].isReliable else { return nil }
        let match = memoryMatch
        guard match.level == .medium || match.level == .strong,
              let record = memory?.records.first(where: { $0.id == match.record }),
              record.proofTeaching else { return nil }
        if record.area == "teaching" { return .teaching }
        if record.area == "dormitory", record.proofDormitory { return .dormitory }
        return nil
    }
    /// Presentation uses the same candidate resolution as a recovery round. It
    /// never grants permission or replaces that round's fresh discovery.
    var currentCampusRouting: CampusRoutingResolution? {
        guard let discovery = lastDiscovery else { return nil }
        let report = campusServiceSelectionReport ?? lastInternetCheck?.base ?? ConnectivityReport(apple: false, microsoft: false)
        let rememberedArea = rememberedCampusArea
        // Preserve the actual decision only while the attachment, candidate and
        // fresh evidence still match the displayed context.
        if routingDiagnosticEpoch == networkEpoch, let actual = lastRoutingDiagnostic,
           actual.choice == area, actual.discovery == discovery,
           actual.rememberedArea == rememberedArea,
           actual.apple == report.appleResult.campusAreaHint,
           actual.microsoft == report.microsoftResult.campusAreaHint {
            return actual.resolution
        }
        return CampusRoutingResolution.resolve(choice: area,
            report: report,
            discovery: discovery, physicallyBound: requiresPhysicalBinding && authenticationRoute != nil,
            rememberedArea: rememberedArea)
    }
    var needsDormitoryRecoveryEligibility: Bool {
        if area != .automatic { return area == .dormitory }
        if let routing = currentCampusRouting {
            switch routing {
            case .selected(let target, _):
                if target != .automatic { return target == .dormitory }
            case .unconfirmed:
                // A candidate hint can require an additional proof without supplying that proof.
                let apple = (campusServiceSelectionReport ?? lastInternetCheck?.base)?.appleResult.campusAreaHint
                let microsoft = (campusServiceSelectionReport ?? lastInternetCheck?.base)?.microsoftResult.campusAreaHint
                guard apple == nil || microsoft == nil || apple == microsoft else { return false }
                return (apple ?? microsoft) == .dormitory
            case .conflictingHints, .needsSelection:
                return false
            }
        }
        return lastDiscovery?.dormitory == .confirmed
            || dormitoryMemoryLevel == .medium || dormitoryMemoryLevel == .strong
    }
    var requiresNetworkConfirmation: Bool {
        guard !currentNetworkAuthorized else { return false }
        if requiresPhysicalBinding && !networkRemembered { return true }
        guard needsDormitoryRecoveryEligibility else { return false }
        return memoryVerdictNow == .notRemembered || memoryVerdictNow == .conflict
    }
    var needsNetworkMemoryUpdate: Bool {
        networkRemembered && needsDormitoryRecoveryEligibility
            && (memoryVerdictNow == .notRemembered || memoryVerdictNow == .conflict)
    }
    var shouldOfferRememberNetwork: Bool {
        !isMaintaining && !sessionReadOnly && !physicalIdentificationNeeded && !sessionOnlyTrust
            && (!networkRemembered || needsNetworkMemoryUpdate)
    }
    /// A bound teaching connection also needs a remembered physical network for recovery.
    var recoverySetupHasNetwork: Bool {
        (networkRemembered && !needsNetworkMemoryUpdate) || (!requiresPhysicalBinding && area == .teaching)
    }
    /// Current-attachment service evidence; session trust and public connectivity never supply proof.
    var campusVerificationState: String {
        guard networkAvailable, !sleeping, let discovery = lastDiscovery else { return "notChecked" }
        let confirmed = discovery.teaching == .confirmed && (area != .dormitory || discovery.dormitory == .confirmed)
        if confirmed { return "confirmed" }
        return discovery.teaching == .notChecked && discovery.dormitory == .notChecked ? "notChecked" : "unconfirmed"
    }
    var requiredCampusVerificationState: String {
        if campusVerificationFailed { return "unconfirmed" }
        if campusVerificationState == "confirmed", needsDormitoryRecoveryEligibility,
           lastDiscovery?.dormitory != .confirmed { return "unconfirmed" }
        return campusVerificationState
    }
    var memoryVerdictNow: MemoryVerdict {
        memoryVerdict(level: dormitoryMemoryLevel, discovery: lastDiscovery ?? CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked),
                      ambiguous: memoryAmbiguous)
    }
    /// "" when remembering is possible; otherwise needsProof | unreliable | readOnly | unreadable.
    var rememberBlocker: String {
        if isMaintaining || sessionReadOnly { return "readOnly" }
        if memoryUnreadable { return "unreadable" }
        if sessionOnlyTrust { return "sessionOnly" }
        guard memory != nil, policy.available, !sleeping, lastLinkActive else { return "needsProof" }
        guard currentIdentity != nil, currentDescriptors.count == 1, currentDescriptors[0].isReliable else {
            return currentDescriptors.isEmpty ? "needsProof" : "unreliable"
        }
        guard let seen = lastDiscovery, seen.teaching == .confirmed,
              area != .dormitory || seen.dormitory == .confirmed else { return "needsProof" }
        return ""
    }
    var canRememberNetwork: Bool { rememberBlocker.isEmpty }

    /// Eligibility to start a credential-free check. No cached discovery is required or reused.
    var canVerifyAndRememberNetwork: Bool {
        !isMaintaining && !credentialsDeletionInProgress && (!requiresPhysicalBinding || authenticationRoute != nil) && !sessionOnlyTrust && !sessionReadOnly && !memoryUnreadable && memory != nil && policy.available && policy.foreground
            && !sleeping && lastLinkActive && currentIdentity != nil
            && currentDescriptors.count == 1 && currentDescriptors[0].isReliable
            && !busy && policy.active == nil
    }
    /// Checking a remembered attachment refreshes evidence only; it does not remember it again.
    var canVerifyCurrentCampusService: Bool { canVerifyAndRememberNetwork && networkRemembered }

    /// An explicit, cancellable setup action. It probes fixed campus services without logging in.
    func verifyAndRememberCurrentNetwork() { startCampusVerification(remember: true) }

    /// A credential-free recheck of the remembered physical attachment. It neither changes memory
    /// nor grants session trust, and completion cannot accelerate or create automatic monitoring.
    func verifyCurrentCampusService() { startCampusVerification(remember: false) }

    private func startCampusVerification(remember: Bool) {
        guard !isMaintaining else { return }
        refreshNetworkEnvironment()
        guard canVerifyAndRememberNetwork, remember || networkRemembered else {
            let reason = sessionReadOnly ? "readOnly" : memoryUnreadable ? "unreadable" : isVerifyingMemory ? "checking" : busy ? "busy"
                : sessionOnlyTrust || currentDescriptors.count > 1 ? "unreliable" : "unavailable"
            setVerificationFeedback("memory.verification." + reason)
            return
        }
        #if os(macOS)
        if remember, let memory, let descriptor = currentDescriptors.first {
            let merging = memory.mergeCandidates(descriptor, now: now())
            if merging.count > 1, networkLabelsUnavailable {
                setVerificationFeedback("memory.verification.writeFailed")
                return
            }
            if let survivor = merging.first {
                var labels = networkLabels
                if case .conflict = labels.preserveMergedMetadata(survivor: survivor, sources: merging) {
                    setVerificationFeedback("memory.verification.nameConflict")
                    return
                }
            }
            if !wifiRecovery.preserveExclusionsBeforeMerging(merging) {
                setVerificationFeedback("memory.verification.writeFailed")
                return
            }
        }
        #endif
        let previousSchedule = CampusVerificationSchedule(wasRunning: loop != nil, deadline: nextCheckAt)
        stopLoop()
        guard let operation = policy.begin(.manual) else { return }
        let diagnosticRound = diagnosticRoundContext(operation: operation)
        let revision = networkRevision
        let generation = memoryGeneration
        let selectedArea = area
        // Independent verification obtains its own fresh hints after transport binding.
        // Old displayed results never choose the protocol for this new operation.
        let rememberedArea = rememberedCampusArea
        let physicallyBound = requiresPhysicalBinding && authenticationRoute != nil
        let descriptors = currentDescriptors
        let identity = currentIdentity
        campusServiceOnlyVerification = !remember
        campusServiceOnlySchedule = remember ? nil : previousSchedule
        busy = true
        isVerifyingMemory = true
        let checkingKey = remember ? "memory.verification.checking" : "campus.verify.checking"
        setVerificationFeedback(checkingKey)
        confirmedCampusServiceContext = nil
        lastDiscovery = nil
        engineStatus = "discovering"
        status = SharedCopy.t("status.discovering.headline")
        authenticationSummary = SharedCopy.t(checkingKey)
        manual = trackedBackgroundTask { [weak self] in
            guard let self else { return }
            var failureResult: String? = "verificationFailed"
            defer {
                if let failureResult, self.policy.owns(operation), !Task.isCancelled {
                    self.recordFailureContext(diagnosticRound, kind: "campusVerification", result: failureResult)
                }
                if self.policy.finish(operation) {
                    self.campusServiceOnlyVerification = false
                    self.campusServiceOnlySchedule = nil
                    self.isVerifyingMemory = false
                    self.busy = false
                    self.manual = nil
                    self.engineStatus = self.isOnline ? "online" : "idle"
                    self.refreshRecovery()
                    if remember {
                        self.startLoop(initialDelay: 2)
                    } else {
                        self.resumeAfterCampusServiceCheck(previousSchedule)
                    }
                }
            }
            guard await self.prepareTransport(for: operation) else {
                failureResult = "identificationUnavailable";
                if self.policy.owns(operation) {
                    let key = remember ? "memory.verification.needsProof" : "campus.verify.failed"
                    self.setVerificationFeedback(key)
                    self.authenticationSummary = SharedCopy.t(key)
                }
                return
            }
            var selectionReport = ConnectivityReport(apple: false, microsoft: false)
            if selectedArea == .automatic {
                selectionReport = await self.client.probe(mode: .quick)
                guard !Task.isCancelled, self.policy.owns(operation), !self.isMaintaining,
                      !self.sleeping, self.networkAvailable, self.networkRevision == revision,
                      self.memoryGeneration == generation, self.currentIdentity == identity,
                      self.currentDescriptors == descriptors, self.area == selectedArea else { return }
            }
            let discovered = await self.client.discoverCampus(area: selectedArea == .teaching ? .teaching : .automatic)
            guard !Task.isCancelled, !self.isMaintaining, !self.credentialsDeletionInProgress,
                  !self.sessionReadOnly, !self.sleeping, self.networkAvailable,
                  self.policy.owns(operation), self.networkRevision == revision,
                  self.memoryGeneration == generation, self.currentIdentity == identity,
                  self.currentDescriptors == descriptors, self.area == selectedArea else { return }
            self.campusServiceSelectionReport = selectionReport
            self.lastDiscovery = discovered
            let resolution = CampusRoutingResolution.resolve(choice: selectedArea, report: selectionReport,
                discovery: discovered, physicallyBound: physicallyBound, rememberedArea: rememberedArea)
            let scopeChoice: CampusArea
            switch resolution {
            case .selected(let candidate, _): scopeChoice = candidate
            case .needsSelection: scopeChoice = selectedArea // Remembering both services does not select a login protocol.
            case .conflictingHints, .unconfirmed:
                self.setVerificationFeedback(remember ? "memory.verification.needsProof" : "campus.verify.failed")
                self.authenticationSummary = self.memoryVerificationMessage ?? ""
                return
            }
            guard let scope = discovered.confirmedCandidate(for: scopeChoice) else {
                let key = remember ? "memory.verification.needsProof" : "campus.verify.failed"
                self.setVerificationFeedback(key)
                self.authenticationSummary = self.memoryVerificationMessage ?? SharedCopy.t(key)
                return
            }
            self.markCampusServiceConfirmed()
            self.campusServiceRetry = nil
            guard remember else {
                failureResult = nil
                self.setVerificationFeedback("campus.verify.confirmed")
                self.authenticationSummary = SharedCopy.t("campus.verify.confirmed")
                return
            }
            switch self.rememberVerifiedNetwork(scope: scope) {
            case .remembered:
                failureResult = nil
                self.markCampusServiceConfirmed()
                self.setVerificationFeedback("memory.verification.remembered")
            case .failed:
                failureResult = "memoryWriteFailed"
                self.setVerificationFeedback("memory.verification.writeFailed")
            case .nameConflict:
                failureResult = "memoryWriteFailed"
                self.setVerificationFeedback("memory.verification.nameConflict")
            default:
                self.setVerificationFeedback("memory.verification.pending")
            }
        }
    }

    private func resumeAfterCampusServiceCheck(_ schedule: CampusVerificationSchedule) {
        guard schedule.wasRunning else { return }
        // Resume only pre-existing monitoring, without advancing its deadline or accelerating
        // recovery. Cancellation follows the same rule as normal check-only completion.
        let remaining = schedule.deadline.map { max(0, Int(ceil($0.timeIntervalSince(now())))) } ?? 0
        startLoop(initialDelay: max(remaining, max(recommendedCheckDelay, 30)))
    }

    /// Complete the remembered-network indication even when public probes already
    /// pass. This remains part of the current check: it never starts authentication,
    /// grants trust, changes memory or reschedules the monitoring loop.
    private func confirmRememberedCampusServiceIfNeeded(_ operation: ConnectionPolicy.Operation,
                                                       report: ConnectivityReport) async {
        guard monitoringMode == .recover, !sessionReadOnly, !isMaintaining, !credentialsDeletionInProgress,
              !sleeping, policy.owns(operation), networkAvailable, physicalNetworkIdentified,
              networkRemembered, !memoryUnreadable, !sessionOnlyTrust,
              currentDescriptors.count == 1, currentDescriptors[0].isReliable else { return }
        let context = currentCampusServiceContext
        if confirmedCampusServiceContext == context, requiredCampusVerificationState == "confirmed",
           let deadline = confirmedCampusServiceDeadline, monotonicNow() < deadline { return }
        if let retry = campusServiceRetry, retry.context == context, monotonicNow() < retry.after { return }
        defer {
            // A manual connectivity check can keep its owner when monitoring is
            // turned off mid-request. Discard that proof and finish its UI phase.
            if policy.owns(operation), context == currentCampusServiceContext {
                if verificationFeedback?.key == "campus.verify.checking" { setVerificationFeedback("campus.verify.cancelled") }
                if engineStatus == "discovering" { engineStatus = isOnline ? "online" : "idle" }
                refreshRecovery()
            }
        }
        // A failed or interrupted request must not repeat on every quick probe.
        // A new path, region or memory generation has its own immediate check.
        campusServiceRetry = (context, monotonicNow() + 30)
        setVerificationFeedback("campus.verify.checking")
        confirmedCampusServiceContext = nil
        confirmedCampusServiceDeadline = nil
        lastDiscovery = nil
        engineStatus = "discovering"
        authenticationSummary = SharedCopy.t("campus.verify.checking")
        refreshRecovery()
        let physicallyBound = requiresPhysicalBinding && authenticationRoute != nil
        let rememberedArea = rememberedCampusArea
        let dormitoryHint = report.appleResult.campusAreaHint == .dormitory || report.microsoftResult.campusAreaHint == .dormitory
        let discoveryArea: CampusArea = context.area == .teaching || (rememberedArea == .teaching && !dormitoryHint) ? .teaching : .automatic
        let discovered = await client.discoverCampus(area: discoveryArea)
        guard !Task.isCancelled, !isMaintaining, !credentialsDeletionInProgress, !sessionReadOnly,
              !sleeping, networkAvailable, policy.owns(operation), context == currentCampusServiceContext,
              monitoringMode == .recover, networkRemembered else { return }
        campusServiceSelectionReport = report
        lastDiscovery = discovered
        let resolution = CampusRoutingResolution.resolve(choice: context.area, report: report,
            discovery: discovered, physicallyBound: physicallyBound, rememberedArea: rememberedArea)
        let choice: CampusArea?
        switch resolution {
        case .selected(let area, _): choice = area
        case .needsSelection: choice = context.area // Service confirmation does not select a login protocol.
        case .conflictingHints, .unconfirmed: choice = nil
        }
        if let choice, discovered.confirmedCandidate(for: choice) != nil {
            markCampusServiceConfirmed()
            campusServiceRetry = nil
            setVerificationFeedback("campus.verify.confirmed")
        } else {
            confirmedCampusServiceContext = nil
            campusServiceRetry = (context, monotonicNow() + 30)
            setVerificationFeedback("campus.verify.autoFailed")
        }
        authenticationSummary = memoryVerificationMessage ?? ""
        engineStatus = "online"
        refreshRecovery()
    }

    private func markCampusServiceConfirmed() {
        confirmedCampusServiceContext = currentCampusServiceContext
        confirmedCampusServiceDeadline = monotonicNow() + 15 * 60
        clearCampusDirectToolFailures()
        updateCampusDirectSuggestion()
    }

    /// Called only by the same-operation fresh verification above; disk commits before authorization.
    private func rememberVerifiedNetwork(scope: CampusArea) -> RememberResult {
        guard rememberBlocker.isEmpty, var next = memory, let descriptor = currentDescriptors.first else { return .needsProof }
        let dormitory = scope == .dormitory
        let rememberedAt = now()
        let merging = Set(next.mergeCandidates(descriptor, now: rememberedAt))
        guard let id = next.remember(descriptor, area: dormitory ? "dormitory" : "teaching",
                                     proofTeaching: true, proofDormitory: dormitory,
                                     now: rememberedAt, newID: NetworkMemory.randomHex()) else { return .unreliable }
        let removed = merging.subtracting(next.records.map(\.id))
        guard removed.isEmpty || !networkLabelsUnavailable else { return .failed }
        var labels = networkLabels
        let metadata = removed.isEmpty ? NetworkLabels.MergeResult.unchanged
            : labels.preserveMergedMetadata(survivor: id, sources: removed.sorted())
        if case .conflict = metadata { return .nameConflict }
        if case .unavailable = metadata { return .failed }
        #if os(macOS)
        guard wifiRecovery.preserveExclusionsBeforeMerging(merging.sorted()) else { return .failed }
        #endif
        // Save the surviving metadata while every source entry still exists. A later memory
        // write failure leaves the old records and labels available for a safe retry.
        if case .updated = metadata, !saveNetworkLabels(labels) { return .failed }
        guard commitMemory(next, suppress: [], restoring: [id]) else { return .failed }
        saveObservedNetworkLabel(ids: [id])
        if dormitory {
            policy.authorizeCurrentNetwork()
            clearAuthorizationGrace()
            refreshAuthorization()
        }
        authenticationSummary = dormitory ? SharedCopy.t("memory.automaticKnown")
            : SharedCopy.t("memory.automaticKnown")
        record(SharedCopy.t("event.remembered"))
        objectWillChange.send()
        return .remembered
    }

    /// Removes only the chosen eligibility. Revoking an unrelated record must not interrupt the current network.
    func forgetNetwork(id: String) {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, !memoryUnreadable, var next = memory,
              rememberedNetworks.contains(where: { $0.id == id }) else { return }
        let ids = next.equivalentIDs(id: id)
        let isCurrent = !Set(next.matchingIDs(currentDescriptors, now: now())).isDisjoint(with: ids)
        guard !ids.isEmpty else { return }
        for removed in ids { next.forget(id: removed) }
        if isCurrent {
            setVerificationFeedback(nil)
            cancelOperations()
            memoryGeneration &+= 1
            revokeAuthorization(reason: SharedCopy.t("event.trustRevoked"))
            authenticationSummary = SharedCopy.t("event.forgot")
        }
        // A failed write still revokes this record in RAM for the rest of this process.
        _ = commitMemory(next, suppress: ids)
        record(SharedCopy.t("event.forgotSelected"))
        objectWillChange.send()
        if isCurrent { startLoop() }
    }

    /// Revokes trust, cancels running work and deletes the records that represent the current network.
    func forgetCurrentNetwork() {
        guard !isMaintaining else { return }
        guard !sessionReadOnly else { return }
        setVerificationFeedback(nil)
        cancelOperations()
        memoryGeneration &+= 1
        revokeAuthorization(reason: SharedCopy.t("event.trustRevoked"))
        if var next = memory {
            let ids = next.matchingIDs(currentDescriptors, now: now())
            for id in ids { next.forget(id: id) }
            if !ids.isEmpty { _ = commitMemory(next, suppress: ids) }
        }
        authenticationSummary = SharedCopy.t("event.forgot")
        record(SharedCopy.t("event.forgot"))
        objectWillChange.send()
        startLoop()
    }

    func forgetAllNetworks() {
        guard !isMaintaining else { return }
        guard !sessionReadOnly else { return }
        setVerificationFeedback(nil)
        cancelOperations()
        memoryGeneration &+= 1
        revokeAuthorization(reason: SharedCopy.t("event.forgotAll"))
        if var next = memory {
            let ids = next.records.map(\.id)
            next.forgetAll()
            if !ids.isEmpty { _ = commitMemory(next, suppress: ids) }
        }
        record(SharedCopy.t("event.forgotAll"))
        objectWillChange.send()
        startLoop()
    }

    private func loadNetworkMemory(_ url: URL) {
        memoryURL = url
        do {
            if let data = try NetworkMemoryFile.read(url) {
                if let decoded = NetworkMemory.decode(data, now: now()), decoded.hasSalt { memory = decoded }
                else { memoryUnreadable = true; errorMessage = SharedCopy.t("memory.unreadable") }
            } else {
                memory = NetworkMemory(salt: NetworkMemory.randomHex())
            }
        } catch {
            memoryUnreadable = true
            errorMessage = SharedCopy.t("memory.unreadable")
        }
        refreshRemembered()
    }

    private func refreshRemembered() {
        rememberedNetworks = (memory?.records ?? []).filter { !suppressedMemory.contains($0.id) }
        var retained = networkLabels
        if retained.retain(ids: Set(rememberedNetworks.map(\.id))) { _ = saveNetworkLabels(retained) }
        updateWiFiRecoveryContext()
    }

    private func loadNetworkLabels(_ url: URL) {
        let store = NetworkLabelsStore(url: url)
        networkLabelsStore = store
        switch store.load() {
        case .loaded(let labels): networkLabels = labels
        case .missing: break
        case .unavailable:
            networkLabelsUnavailable = true
            networkNameError = SharedCopy.t("memory.namesUnavailable")
        }
    }

    /// Called with metadata from the same OS snapshot as the current, reliable physical path.
    func observeNetworkLabel(_ label: NetworkObservedLabel?) {
        guard !isMaintaining else { return }
        observedNetworkLabel = label
        guard label != nil, currentIdentity != nil, lastLinkActive, policy.available,
              currentDescriptors.count == 1, currentDescriptors[0].isReliable,
              !sleeping, let memory, !memoryUnreadable else { return }
        let ids = memory.matchingIDs(currentDescriptors, now: now()).filter { !suppressedMemory.contains($0) }
        saveObservedNetworkLabel(ids: ids)
    }

    /// A connected secondary Wi-Fi can provide its own name; it never supplies egress or login authority.
    private func observeAttachedWiFiNames(_ snapshot: NetworkContinuitySnapshot) {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, !sleeping, !networkLabelsUnavailable, !memoryUnreadable,
              snapshot.hasUsablePhysicalLink, let memory else { return }
        var next = networkLabels
        var changed = false
        for attachment in snapshot.wifiLabelAttachments.prefix(NetworkMemory.limit) {
            guard attachment.descriptor.kind == "wifi", attachment.descriptor.isReliable,
                  attachment.label.kind == .wifi, attachment.label.ssid != nil else { continue }
            let matches = memory.matchingIDs([attachment.descriptor], now: now()).filter { !suppressedMemory.contains($0) }
            guard matches.count == 1, let id = matches.first else { continue }
            changed = next.updateAutomatic(id: id, displayName: attachment.label.displayName,
                                           kind: .wifi, ssid: attachment.label.ssid) || changed
        }
        if changed { _ = saveNetworkLabels(next) }
    }

    private func saveObservedNetworkLabel(ids: [String]) {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, !networkLabelsUnavailable, let label = observedNetworkLabel else { return }
        var next = networkLabels
        var changed = false
        for id in ids {
            changed = next.updateAutomatic(id: id, displayName: label.displayName, kind: label.kind, ssid: label.ssid) || changed
        }
        if changed { _ = saveNetworkLabels(next) }
    }

    @discardableResult
    private func saveNetworkLabels(_ labels: NetworkLabels) -> Bool {
        guard !isMaintaining else { return false }
        guard !sessionReadOnly, !networkLabelsUnavailable else { return false }
        if let store = networkLabelsStore, !store.save(labels) {
            networkNameError = SharedCopy.t("memory.namesWriteFailed")
            return false
        }
        networkLabels = labels
        networkNameError = nil
        updateWiFiRecoveryContext()
        return true
    }

    func renameRememberedNetwork(id: String, name: String) -> Bool {
        guard !isMaintaining else { return false }
        guard !sessionReadOnly, rememberedNetworks.contains(where: { $0.id == id }) else { return false }
        var next = networkLabels
        guard next.rename(id: id, to: name) else {
            return next.displayName(for: id) == NetworkLabels.cleanName(name) && !NetworkLabels.cleanName(name).isEmpty
        }
        return saveNetworkLabels(next)
    }

    var hasUnnamedRememberedNetworks: Bool {
        rememberedNetworks.contains { networkLabels.entries[$0.id] == nil }
    }

    /// Display names never establish identity or merge separate records.
    func rememberedNetworkDisplayName(for id: String) -> String { networkLabels.displayName(for: id) }

    /// One notice per group preserves the original names and each record's own controls.
    var rememberedNetworkSameNameNoticeIDs: Set<String> {
        let counts = Dictionary(grouping: rememberedNetworks, by: { networkLabels.displayName(for: $0.id) })
        var seen = Set<String>()
        return Set(rememberedNetworks.compactMap { record in
            let name = networkLabels.displayName(for: record.id)
            guard (counts[name]?.count ?? 0) > 1, seen.insert(name).inserted else { return nil }
            return record.id
        })
    }

    /// A presentation-only marker for one unambiguous, reliable live attachment.
    /// A best-ranked memory match alone is insufficient when several records or links match.
    var currentRememberedNetworkID: String? {
        guard !isMaintaining, !networkInformationRefreshing, !memoryUnreadable, !networkLabelsUnavailable,
              !sessionOnlyTrust, policy.available, !sleeping, currentIdentity != nil, lastLinkActive,
              networkRemembered, !hasMultipleNetworkAttachments, currentDescriptors.count == 1,
              let descriptor = currentDescriptors.first, descriptor.isReliable, let memory else { return nil }
        if let physicalDiagnostics, physicalDiagnostics.candidateCount != 1 { return nil }
        if requiresPhysicalBinding {
            guard let route = authenticationRoute, route.descriptor == descriptor else { return nil }
        }
        let visible = Set(rememberedNetworks.map(\.id))
        let matches = memory.matchingIDs([descriptor], now: now()).filter {
            !suppressedMemory.contains($0) && visible.contains($0)
        }
        return matches.count == 1 ? matches[0] : nil
    }

    func rememberedNetworkDetail(for record: MemoryRecord) -> String {
        let kind = networkLabels.entries[record.id]?.kind.rawValue ?? "other"
        var parts = [String]()
        if currentRememberedNetworkID == record.id { parts.append(SharedCopy.t("memory.currentConnection")) }
        parts.append(SharedCopy.t("memory.kind." + kind))
        if record.area == "dormitory" || record.area == "teaching" { parts.append(SharedCopy.t("memory.entry." + record.area)) }
        return parts.joined(separator: " · ")
    }

    private func updateWiFiRecoveryContext() {
        #if os(macOS)
        guard initialized, !isMaintaining else { return }
        let allowed = rememberedNetworks.compactMap { record -> WiFiRecoveryNetwork? in
            guard record.verified + NetworkMemory.expiry >= now().timeIntervalSince1970,
                  let label = networkLabels.entries[record.id], let ssid = label.ssid else { return nil }
            return WiFiRecoveryNetwork(id: record.id, ssid: ssid, displayName: rememberedNetworkDisplayName(for: record.id))
        }
        wifiRecovery.updateContext(allowRecovery: !sessionReadOnly && monitoringMode == .recover
            && automaticCredentials != nil && policy.foreground && policy.holds.holds.isEmpty && !busy
            && !networkLabelsUnavailable && !memoryUnreadable,
            hasPhysicalConnection: hasPhysicalConnection, sleeping: sleeping, allowedNetworks: allowed)
        #endif
    }

    /// Atomic write first; the in-memory copy changes only after the file did. A failed write keeps the old file and
    /// (for a revocation) suppresses the affected records for this session.
    private func commitMemory(_ proposed: NetworkMemory, suppress: [String], restoring: Set<String> = []) -> Bool {
        guard !isMaintaining else { return false }
        guard !sessionReadOnly else { return false }
        var next = proposed
        let revoked = suppressedMemory.union(suppress).subtracting(restoring)
        for id in revoked { next.forget(id: id) }
        guard let url = memoryURL else {
            memory = next
            suppressedMemory.subtract(restoring)
            refreshRemembered()
            return true
        }
        do {
            try NetworkMemoryFile.write(next.encoded(), to: url)
            memory = next
            suppressedMemory.subtract(restoring)
            memoryWriteFailed = false
            refreshRemembered()
            return true
        } catch {
            suppressedMemory.formUnion(suppress)
            memoryWriteFailed = true
            errorMessage = SharedCopy.t("memory.writeFailed")
            refreshRemembered()
            return false
        }
    }

    private func touchMemory(area: CampusArea, authenticated: Bool = false) {
        guard !isMaintaining else { return }
        guard var next = memory else { return }
        let found = area == .dormitory
            ? next.matchAny(currentDescriptors, now: now(), exclude: suppressedMemory, requireDormitoryProof: true)
            : memoryMatch
        guard !found.record.isEmpty, let previous = next.records.first(where: { $0.id == found.record }) else { return }
        // Homepage reachability refreshes proof age, not the preferred login protocol.
        // Only an accepted authentication can replace the remembered entry choice.
        let preferredArea = authenticated ? (area == .dormitory ? "dormitory" : "teaching") : previous.area
        if authenticated, area == .teaching, previous.proofDormitory {
            // Teaching success cannot extend an older dormitory proof's expiry.
            guard next.preferAuthenticatedArea(id: found.record, area: preferredArea) else { return }
        } else {
            guard next.touch(id: found.record, now: now(), area: preferredArea) else { return }
        }
        _ = commitMemory(next, suppress: [])
    }

    func authorizeCurrentNetwork() {
        guard !isMaintaining else { return }
        refreshNetworkEnvironment()
        guard canTrustCurrentNetwork && policy.foreground else {
            if sessionReadOnly {
                authorizationReason = SharedCopy.t("memory.hint.readOnly")
            } else if policy.available && currentIdentity == nil && lastLinkActive {
                authorizationReason = SharedCopy.t("guard.trustUnavailable")
            } else {
                authorizationReason = SharedCopy.t("memory.hint.needsProof")
            }
            refreshAuthorization()
            return
        }
        policy.authorizeCurrentNetwork()
        clearAuthorizationGrace()
        refreshAuthorization()
        authenticationSummary = SharedCopy.t("auto.ready.detail")
        record(SharedCopy.t("event.trusted"))
        if rapidRecoveryDeadline != nil { rapidRecoveryDeadline = now().addingTimeInterval(45) }
        if engineStatus == "needsTrust" { engineStatus = "idle" }
        if policy.active == nil {
            stopLoop()
            startLoop(initialDelay: 0)
        }
    }
    func revokeCurrentNetworkAuthorization() {
        guard !isMaintaining else { return }
        cancelOperations()
        revokeAuthorization(reason: SharedCopy.t("event.trustRevoked"))
        authenticationSummary = SharedCopy.t("event.trustRevoked")
        startLoop()
    }
    func resumeAutomaticRecovery() {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, !credentialsDeletionInProgress, monitoringMode == .recover, automaticCredentials != nil else { return }
        cancelOperations()
        if recoveryJournalBlocked, let url = recoveryJournalURL {
            do {
                if FileManager.default.fileExists(atPath: url.path) { _ = try EventLogFile.quarantine(url) }
                recoveryJournalBlocked = false; lastRecoveryJournal = nil
            } catch { errorMessage = SharedCopy.t("error.recoveryJournalBackupFailed"); return }
        }
        pendingRecoverySubmission = false
        policy.resumeAuthentication()
        authenticationSummary = SharedCopy.t("event.resumed")
        refreshRecovery()
        startLoop()
    }
    /// Explicit user pause of automatic login (checks continue). Resume clears every hold.
    func pauseAutomaticRecovery() {
        guard !isMaintaining else { return }
        guard monitoringMode == .recover else { return }
        if policy.active?.kind == .automatic { cancelOperations() }
        policy.pauseByUser()
        clearPriorityRecovery()
        record(SharedCopy.t("event.paused"))
        refreshRecovery()
        startLoop()
    }
    /// Automatic submission is currently paused for the configured area (AUTO-RECOVERY-01).
    var pausedForAutomatic: Bool {
        guard monitoringMode == .recover, automaticCredentials != nil else { return false }
        let generation = savedGeneration
        if policy.holds.globalHold(generation: generation, epoch: networkEpoch) != nil { return true }
        if area == .automatic {
            return policy.holds.blocking(.teaching, generation: generation, epoch: networkEpoch) != nil
                && policy.holds.blocking(.dormitory, generation: generation, epoch: networkEpoch) != nil
        }
        return policy.holds.blocking(area, generation: generation, epoch: networkEpoch) != nil
    }
    var strongestHoldMessage: String {
        policy.holds.strongest.map { ConnectionPolicy.AuthenticationBlock($0.reason).message } ?? SharedCopy.t("status.paused.detail")
    }
    private var authenticationScope: PhysicalAuthenticationScope? {
        guard requiresPhysicalBinding, let route = authenticationRoute, let descriptor = route.descriptor,
              descriptor.isReliable else { return nil }
        let scope = PhysicalAuthenticationScope(kind: descriptor.kind, interface: route.interfaceName)
        return scope.valid ? scope : nil
    }
    var cooldownRemaining: TimeInterval? { policy.cooldownRemaining(now: now(), scope: authenticationScope) }
    var canTrustCurrentNetwork: Bool {
        !isMaintaining && (!requiresPhysicalBinding || authenticationRoute != nil) && !sessionReadOnly && policy.available && !sleeping && currentIdentity != nil && lastLinkActive
    }
    private func credentialGeneration(for credentials: Credentials) -> UInt64? {
        guard let saved = automaticCredentials, saved.account == credentials.account, saved.password == credentials.password else { return nil }
        return savedGeneration
    }

    func cancelCurrentOperation() {
        guard !isMaintaining else { return }
        let cancellingSnapshot = manualSnapshotID != nil
        let snapshotDeadline = manualResumeDeadline
        // Cancel this round only; automatic monitoring may resume after a quiet interval.
        let serviceCheckSchedule = campusServiceOnlyVerification ? campusServiceOnlySchedule : nil
        if isVerifyingMemory {
            setVerificationFeedback(campusServiceOnlyVerification ? "campus.verify.cancelled" : "memory.verification.cancelled")
        }
        cancelOperations()
        status = isOnline ? SharedCopy.t("diagnosis.online.title") : SharedCopy.t("status.cancelled.headline")
        engineStatus = isOnline ? "online" : "cancelled"
        detail = SharedCopy.t("status.cancelled.detail")
        authenticationSummary = SharedCopy.t("status.cancelled.detail")
        if cancellingSnapshot {
            let quietDeadline = monotonicNow() + 30
            resumeAfterManualSnapshot(max(snapshotDeadline ?? quietDeadline, quietDeadline))
        }
        else if let serviceCheckSchedule { resumeAfterCampusServiceCheck(serviceCheckSchedule) }
        else { startLoop(initialDelay: 30) }
        refreshRecovery()
    }
    func setAutoReconnect(_ enabled: Bool) {
        setMonitoringMode(enabled ? .recover : .manual)
    }
    func setMonitoringMode(_ mode: MonitoringMode) {
        guard !isMaintaining else { return }
        guard monitoringMode != mode else { return }
        diagnosticCancellation &+= 1
        manualActionRevision &+= 1
        invalidateManualSnapshot()
        if mode != .recover, !isVerifyingMemory, verificationFeedback?.key == "campus.verify.checking" {
            // This online confirmation belongs to recovery monitoring, even if
            // its public probe was started by the manual check button.
            cancelOperations()
        }
        if mode != .recover { clearPriorityRecovery() }
        sessionAuditDeadline = nil
        partialRecovery.resetConfirmation()
        monitoringMode = mode
        autoReconnect = mode == .recover
        policy.automaticEnabled = mode != .manual
        defaults?.set(mode.rawValue, forKey: "monitoringMode")
        defaults?.set(autoReconnect, forKey: "autoReconnect")
        // Switching to read-only cancels every request. Other modes preserve a manual operation.
        if mode == .observe { cancelOperations() }
        else {
            if policy.active?.kind == .automatic { preservePendingSubmission() }
            stopLoop()
            policy.cancel(.automatic)
            currentNetworkAuthorized = policy.currentNetworkAuthorized
            busy = policy.active != nil
        }
        if mode == .manual && !isOnline {
            status = SharedCopy.t("status.idle.headline"); detail = SharedCopy.t("mode.manual.detail")
        }
        authenticationSummary = mode == .recover ? SharedCopy.t("auto.ready.detail") : SharedCopy.t("auto.disabledDetail")
        // Mode switches never release a credential or security rejection block.
        if mode != .manual { policy.resetFailures(); startLoop(initialDelay: canPrioritizeRecovery ? 0 : 2) }
        record(SharedCopy.f("event.modeChanged", ["mode": mode.title]))
        refreshRecovery()
    }
    func setAlwaysShowDockIcon(_ enabled: Bool) {
        guard !isMaintaining else { return }
        guard !sessionReadOnly, dockPreferenceAvailable, alwaysShowDockIcon != enabled else { return }
        dockVisibilityError = nil
        dockPreferences?.set(enabled, forKey: "alwaysShowDockIcon")
        alwaysShowDockIcon = enabled
    }
    func reportDockVisibilityFailure() {
        let message = SharedCopy.t("settings.dock.failed")
        if dockVisibilityError != message { dockVisibilityError = message }
    }
    func setLaunchAtLogin(_ enabled: Bool) {
        guard !isMaintaining else { return }
        #if os(macOS)
        guard !sessionReadOnly, !isolated else { return }
        do {
            if enabled { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
            launchAtLogin = SMAppService.mainApp.status == .enabled
            if enabled && !launchAtLogin { errorMessage = SharedCopy.t("error.launchApproval") }
        } catch {
            launchAtLogin = SMAppService.mainApp.status == .enabled
            errorMessage = SharedCopy.t("error.launchUpdate")
        }
        #endif
    }
    func setForeground(_ active: Bool) {
        guard !isMaintaining else { return }
        if !active { invalidateManualSnapshot(resumeMonitoring: true) }
        #if os(macOS)
        if active { refreshNetworkEnvironment() }
        #endif
        #if os(iOS)
        guard policy.foreground != active else { return }
        policy.foreground = active
        cancelOperations()
        policy.resetForPathChange()
        revokeAuthorization(reason: "应用进入过后台，回到前台后请重新确认当前网络。")
        if active { startLoop() }
        else { isOnline = false; status = "已暂停"; detail = "回到应用后继续检测。" }
        refreshRecovery()
        #endif
    }
    func setSleeping(_ value: Bool) {
        guard !isMaintaining else { return }
        guard sleeping != value else { return }
        let interruptedMemoryVerification = isVerifyingMemory
        let interruptedServiceOnly = campusServiceOnlyVerification
        sleeping = value
        _ = diagnosticFreshness.invalidate()
        partialRecovery.resetConfirmation()
        updateWiFiRecoveryContext()
        invalidateTransport()
        networkRevision &+= 1
        lastInternetCheck = nil
        websiteConfigurationChanged = false
        networkEpoch &+= 1
        policy.pathChanged(to: networkEpoch)
        engineStatus = value ? "sleeping" : "idle"
        cancelOperations()
        if value && interruptedMemoryVerification {
            setVerificationFeedback(interruptedServiceOnly ? "campus.verify.sleeping" : "memory.verification.sleeping")
        }
        policy.resetForPathChange()
        revokeAuthorization(reason: SharedCopy.t("guard.afterSleep"))
        currentIdentity = nil
        lastLinkActive = false
        policy.available = false // A pre-sleep snapshot cannot authorize the post-wake network.
        isOnline = false
        lastCheckedAt = nil
        lastDiagnosis = nil
        status = value ? SharedCopy.t("status.sleeping.headline") : SharedCopy.t("status.waking.headline")
        detail = value ? SharedCopy.t("status.sleeping.detail") : SharedCopy.t("status.waking.detail")
        probeSummary = SharedCopy.t("internet.pending.title")
        authenticationSummary = SharedCopy.t("guard.afterSleep")
        if !value, !isolated, let path = monitor?.currentPath {
            let token = diagnosticFreshness.invalidate()
            let observation = PathObservation.capture(path)
            applyDiagnosticObservation(token: token) { receive(observation) }
            authorizationReason = SharedCopy.t("guard.afterWake")
        }
        refreshAuthorization()
        refreshRecovery()
    }
    private func stopLoop() {
        loop?.cancel(); loop = nil; loopID = nil; nextCheckAt = nil
    }
    /// Unexpected waiter errors cannot silently disable monitoring or advance its deadline.
    /// Cancellation always stops. The fallback is the native cancellable waiter in production.
    private static func waitMonitoringDelay(_ seconds: Int, sleep: @Sendable (Int) async throws -> Void,
                                           fallback: @Sendable (TimeInterval) async throws -> Void,
                                           clock: () -> TimeInterval) async -> Bool {
        let deadline = clock() + TimeInterval(seconds)
        do { try await sleep(seconds); return !Task.isCancelled }
        catch {
            guard !Task.isCancelled, !(error is CancellationError) else { return false }
            let remaining = max(0, deadline - clock())
            guard remaining.isFinite else { return false }
            do { try await fallback(remaining); return !Task.isCancelled } catch { return false }
        }
    }
    private func startLoop(initialDelay: Int = 2) {
        guard !isMaintaining else { return }
        guard !credentialsDeletionInProgress, manualSnapshotID == nil, automaticLoopAllowed, policy.canRunAutomatic, !sleeping, loop == nil else { return }
        let id = UUID()
        loopID = id
        nextCheckAt = now().addingTimeInterval(TimeInterval(initialDelay))
        let sleepLoop = loopSleep
        let fallback = loopFallbackSleep
        let clock = monotonicNow
        loop = trackedBackgroundTask { [weak self] in
            defer {
                if let self, self.loopID == id {
                    self.loop = nil; self.loopID = nil; self.nextCheckAt = nil
                }
            }
            guard await Self.waitMonitoringDelay(initialDelay, sleep: sleepLoop, fallback: fallback, clock: clock) else { return }
            while !Task.isCancelled {
                guard let self, self.loopID == id, self.policy.canRunAutomatic, !self.sleeping else { return }
                self.nextCheckAt = nil
                await self.runAutomaticCycle()
                guard !Task.isCancelled, self.loopID == id else { return }
                let delay = self.recommendedCheckDelay
                self.nextCheckAt = self.now().addingTimeInterval(TimeInterval(delay))
                self.refreshRecovery()
                guard await Self.waitMonitoringDelay(delay, sleep: sleepLoop, fallback: fallback, clock: clock) else { return }
            }
        }
    }
    /// Also used by the offline integration harness with a fake transport.
    func runAutomaticCycle() async {
        guard !isMaintaining else { return }
        guard !credentialsDeletionInProgress, manualSnapshotID == nil else { return }
        await refreshNetworkEnvironmentInBackground()
        guard !Task.isCancelled, manualSnapshotID == nil, let operation = policy.begin(.automatic), !sleeping else { return }
        if !systemConnectivityChecking { refreshSystemConnectivity() }
        busy = true
        await perform(operation, mayAuthenticate: !sessionReadOnly && monitoringMode == .recover)
    }
    /// A cancelled request may already have reached the server. Keep RAM and the recovery
    /// journal equally conservative until a verified connection or explicit resume settles it.
    private func preservePendingSubmission() {
        if pendingRecoverySubmission { policy.suspendAuthentication(.uncertain) }
    }
    private func cancelOperations() {
        diagnosticCancellation &+= 1
        manualActionRevision &+= 1
        invalidateManualSnapshot()
        resetCampusDirectFailures()
        activeInternetCheck = nil
        invalidateSystemConnectivity()
        if verificationFeedback?.key == "campus.verify.checking" || (isVerifyingMemory && (memoryVerificationMessage == SharedCopy.t("memory.verification.checking")
            || memoryVerificationMessage == SharedCopy.t("campus.verify.checking")
            || memoryVerificationMessage == SharedCopy.t("memory.verification.pending"))) {
            setVerificationFeedback(campusServiceOnlyVerification || verificationFeedback?.serviceOnly == true ? "campus.verify.cancelled" : "memory.verification.cancelled")
        }
        campusServiceOnlyVerification = false
        campusServiceOnlySchedule = nil
        preservePendingSubmission()
        clearPriorityRecovery()
        partialRecovery.resetConfirmation()
        partialRecoveryInProgress = false
        stopLoop()
        manual?.cancel(); manual = nil
        isVerifyingMemory = false
        policy.cancel()
        probeOwner = nil
        refreshAuthorization()
        busy = false
        offerForcedLogin = false
        if engineStatus == "discovering" { engineStatus = "idle" }
    }
    /// Cancel only the acquisition owner. A new click never queues another login,
    /// and a cancelled/failed acquisition cannot bring monitoring's deadline forward.
    private func invalidateManualSnapshot(resumeMonitoring: Bool = false) {
        guard manualSnapshotID != nil else { return }
        let deadline = manualResumeDeadline
        manualSnapshotGeneration &+= 1
        manualSnapshotID = nil
        manualSnapshotTask?.cancel(); manualSnapshotTask = nil
        manualResumeDeadline = nil
        if policy.active == nil { busy = false }
        if resumeMonitoring { resumeAfterManualSnapshot(deadline) }
    }
    private func resumeAfterManualSnapshot(_ deadline: TimeInterval?) {
        let remaining = deadline.map { max(0, $0 - monotonicNow()) } ?? TimeInterval(recommendedCheckDelay)
        startLoop(initialDelay: Int(min(86_400, ceil(remaining))))
    }
    /// Closing a window cancels an unfinished manual read, while normal background
    /// monitoring and an already admitted credential operation keep their lifecycle.
    func cancelPendingManualNetworkRead() {
        manualActionRevision &+= 1
        invalidateManualSnapshot(resumeMonitoring: true)
    }

    private func startManual(mayAuthenticate: Bool, confirmation: ForcedLoginConfirmation? = nil) {
        guard !isMaintaining, !credentialsDeletionInProgress, manualSnapshotID == nil,
              policy.foreground, !sleeping else { return }
        if let confirmation, !forcedLoginContextMatches(confirmation) { return }
        if let active = policy.active, active.kind == .automatic, probeOwner == active {
            // Give explicit user actions priority over a credential-free automatic probe.
            // A portal discovery, login or post-login verification is never preempted here.
            if policy.active?.kind == .automatic { preservePendingSubmission() }
            stopLoop()
            policy.cancel(.automatic)
            probeOwner = nil
            busy = false
        }
        guard policy.active == nil else { return }
        // Isolated fixtures without a reader already supply an explicit observation.
        // Production always captures on a worker before admitting the manual operation.
        if isolated && manualNetworkReader == nil {
            admitManual(mayAuthenticate: mayAuthenticate, confirmation: confirmation)
            return
        }
        let id = UUID()
        manualSnapshotGeneration &+= 1
        let generation = manualSnapshotGeneration
        let configuration = configurationRevision
        manualResumeDeadline = nextCheckAt.map { monotonicNow() + max(0, $0.timeIntervalSince(now())) }
        manualSnapshotID = id
        let diagnosticToken = diagnosticFreshness.invalidate()
        stopLoop()
        busy = true
        manualSnapshotTask = trackedBackgroundTask { [weak self] in
            guard let self else { return }
            do {
                let observation: PathObservation
                if let reader = self.manualNetworkReader { observation = try await reader() }
                else if let path = self.monitor?.currentPath {
                    observation = await Task.detached(priority: .userInitiated) { PathObservation.capture(path) }.value
                } else { throw CocoaError(.fileReadUnknown) }
                guard !Task.isCancelled, self.manualSnapshotID == id,
                      self.manualSnapshotGeneration == generation, !self.isMaintaining, !self.sleeping,
                      self.configurationRevision == configuration, self.policy.active == nil else { return }
                let deadline = self.manualResumeDeadline
                // Release the acquisition owner before applying its own observation;
                // a real route change still invalidates every old operation and proof.
                self.manualSnapshotID = nil; self.manualSnapshotTask = nil; self.manualResumeDeadline = nil
                self.busy = false
                self.manualNetworkReadError = nil
                if let confirmation, !self.forcedLoginContextMatches(confirmation) {
                    self.resumeAfterManualSnapshot(deadline); return
                }
                self.applyDiagnosticObservation(token: diagnosticToken) { self.applyEnvironmentObservation(observation) }
                guard !Task.isCancelled, self.configurationRevision == configuration,
                      !self.isMaintaining, !self.sleeping, self.policy.active == nil,
                      confirmation.map(self.forcedLoginContextMatches) ?? true else {
                    self.resumeAfterManualSnapshot(deadline); return
                }
                self.admitManual(mayAuthenticate: mayAuthenticate, confirmation: confirmation)
                if self.policy.active == nil { self.resumeAfterManualSnapshot(deadline) }
            } catch {
                guard self.manualSnapshotID == id, self.manualSnapshotGeneration == generation else { return }
                let deadline = self.manualResumeDeadline
                self.manualSnapshotID = nil; self.manualSnapshotTask = nil; self.manualResumeDeadline = nil
                self.busy = self.policy.active != nil
                if !Task.isCancelled, !self.isMaintaining, !self.sleeping {
                    self.manualNetworkReadError = SharedCopy.t("env.refreshFailed")
                    self.resumeAfterManualSnapshot(deadline)
                }
            }
        }
    }
    private func admitManual(mayAuthenticate: Bool, confirmation: ForcedLoginConfirmation?) {
        guard !isMaintaining, !credentialsDeletionInProgress, !sleeping, policy.foreground,
              policy.active == nil, confirmation.map(forcedLoginContextMatches) ?? true else { return }
        refreshSystemConnectivity()
        guard policy.available else {
            status = SharedCopy.t("env.access.none"); detail = SharedCopy.t("status.noNetwork.detail"); return
        }
        partialRecovery.resetConfirmation()
        stopLoop()
        if mayAuthenticate { invalidateTransport() }
        guard let operation = policy.begin(.manual) else { return }
        busy = true
        offerForcedLogin = false
        manual = trackedBackgroundTask { [weak self] in
            guard let self else { return }
            await self.perform(operation, mayAuthenticate: mayAuthenticate, force: confirmation != nil)
            if self.policy.active == nil { self.startLoop(initialDelay: self.recommendedCheckDelay) }
        }
    }
    private func perform(_ operation: ConnectionPolicy.Operation, mayAuthenticate requestedAuthentication: Bool,
                         force: Bool = false) async {
        let supplementalCheck = operation.kind == .automatic && recoveryFullCheckPending && probeMode == .complete
        let mayAuthenticate = requestedAuthentication && !supplementalCheck
        // Immutable operation inputs; configuration edits invalidate the operation owner.
        let automaticAreaChoice = area == .automatic
        let authenticationArea = area
        let rememberedArea = rememberedCampusArea
        // A quick success proves only one public endpoint. Resolve an uncertain
        // submission with a complete, read-only round without changing the preference.
        let awaitingSubmissionProof = pendingRecoverySubmission || policy.holds.holds.contains { $0.reason == .uncertain }
        let prioritizedRound = operation.kind == .automatic && !awaitingSubmissionProof && canPrioritizeRecovery
        let quickRecovery = operation.kind == .automatic && mayAuthenticate && probeMode == .quick
            && requiresPhysicalBinding && authenticationRoute != nil && automaticCredentials != nil
            && (networkRemembered || policy.currentNetworkAuthorized) && !pausedForAutomatic
        let sessionAudit = quickRecovery && sessionAuditDeadline.map { monotonicNow() >= $0 } == true
        let selectedProbeMode: ConnectivityProbeMode = awaitingSubmissionProof || sessionAudit
            ? .complete : (prioritizedRound ? .quick : probeMode)
        let selectedWebsites = customWebsiteTargets
        let selectedWebsiteStrategy = customWebsiteStrategy
        let diagnosticRound = diagnosticRoundContext(operation: operation)
        let checkRevision = networkRevision
        let memoryGen = memoryGeneration
        let credentials = operation.kind == .automatic ? automaticCredentials
            : Credentials(account: account.trimmingCharacters(in: .whitespacesAndNewlines), password: password)
        let hasCredentials = !(credentials?.account.isEmpty ?? true) && !(credentials?.password.isEmpty ?? true)
        var failedRound = false
        var diagnosticLatestReport: InternetCheckReport?
        var diagnosticAttemptFinished = false
        var diagnosticBindingFailed = false
        defer {
            if probeOwner == operation { probeOwner = nil }
            if credentialOperation == operation { credentialOperation = nil }
            if policy.owns(operation), !Task.isCancelled, failedRound, operation.kind == .automatic { policy.failed() }
            if policy.owns(operation), !Task.isCancelled {
                let failed = diagnosticLatestReport.map { $0.verdict == .unavailable || $0.verdict == .partial } ?? false
                let verificationFailed = diagnosticDiscoveryRevision != diagnosticRound.discoveryRevision
                    && lastDiscovery != nil && requiredCampusVerificationState != "confirmed"
                if failed || verificationFailed || diagnosticBindingFailed || (diagnosticAttemptFinished && ["rejected", "uncertain", "security", "unavailable", "accepted"].contains(engineStatus)) {
                    recordFailureContext(diagnosticRound, kind: "connection", result: diagnosticBindingFailed ? "identificationUnavailable" : "operationFailed")
                }
            }
            if policy.finish(operation) {
                partialRecoveryInProgress = false
                busy = false
                if operation.kind == .manual { manual = nil }
                refreshRecovery()
            }
        }
        guard await prepareTransport(for: operation) else { diagnosticBindingFailed = true; return }
        if !isOnline || operation.kind == .manual { status = SharedCopy.t("status.checking.headline"); engineStatus = "checking" }
        probeOwner = operation
        let boundSubmissionProof = (awaitingSubmissionProof || sessionAudit) && requiresPhysicalBinding && authenticationRoute != nil
        let internet = await runInternetCheck(mode: selectedProbeMode, targets: selectedWebsites, strategy: selectedWebsiteStrategy,
            prioritizeCampusRecovery: operation.kind == .automatic && mayAuthenticate && requiresPhysicalBinding && authenticationRoute != nil,
            deferWebsites: boundSubmissionProof, diagnosticContext: diagnosticRound.sample,
            diagnosticInterface: diagnosticRound.interface)
        guard !Task.isCancelled, policy.owns(operation), checkRevision == networkRevision else { return }
        if probeOwner == operation { probeOwner = nil }
        let report = internet.base
        diagnosticLatestReport = internet
        acceptReport(report, internet: internet)
        if selectedProbeMode == .complete { recoveryFullCheckPending = false }
        if report.online, rapidRecoveryDeadline != nil {
            rapidRecoveryDeadline = nil
            recoveryFullCheckPending = prioritizedRound && probeMode == .complete
        }
        if operation.kind == .automatic && internet.websitesDeferred && internet.completeBasePassed && probeMode == .complete {
            recoveryFullCheckPending = true
        }
        let boundCampusRedirect = requiresPhysicalBinding && authenticationRoute != nil
            && (report.appleResult.campusAreaHint != nil || report.microsoftResult.campusAreaHint != nil)
        let boundAuthenticationRequired = requiresPhysicalBinding && authenticationRoute != nil
            && (report.appleResult.authenticationRequired || report.microsoftResult.authenticationRequired)
        let partial = operation.kind == .automatic && boundCampusRedirect
            ? boundAuthenticationRequired : (operation.kind == .automatic ? internet.hasCampusRecoveryEvidence : internet.hasRecoveryEvidence)
        if operation.kind == .automatic {
            // Quick monitoring excludes optional websites. A periodic base-only
            // check must observe both endpoints, including the peer quick mode skips.
            partialRecovery.observe(partial: partial,
                transportRecovered: internet.transportRecovered || (sessionAudit && internet.completeBasePassed))
        }
        if quickRecovery {
            if boundCampusRedirect || !report.online || (sessionAudit && !internet.completeBasePassed) {
                sessionAuditDeadline = monotonicNow()
            } else if sessionAuditDeadline == nil || internet.completeBasePassed {
                sessionAuditDeadline = monotonicNow() + (partialRecovery.needsTransportConfirmation ? 0 : 60)
            }
        }
        let forced = force && operation.kind == .manual && mayAuthenticate
        let partialAutomatic = operation.kind == .automatic && partial && partialRecovery.shouldAttempt && mayAuthenticate
        let partialManual = operation.kind == .manual && partial && mayAuthenticate
        if report.online {
            policy.observeProbe(online: true)
            setOnline()
            guard forced || partialAutomatic || partialManual else {
                if partial, monitoringMode == .recover, !sessionReadOnly {
                    authenticationSummary = partialRecovery.attempted
                        ? SharedCopy.t("internet.partial.afterAttempt")
                        : SharedCopy.t("internet.partial.confirming")
                } else if operation.kind == .manual, mayAuthenticate, canAuthenticate, hasCredentials {
                    offerForcedLogin = true
                    detail = SharedCopy.f("status.online.noSubmission", ["action": SharedCopy.t("action.forceLogin")])
                    if internet.hasOnlyWebsiteIssues {
                        detail = SharedCopy.t("internet.websiteIssues.detail") + "\n\n" + detail
                    }
                }
                // Keep observed base failures and portal redirects in the existing
                // complete-check/recovery flow. A normal quick check may skip one endpoint.
                let healthyBase = [report.appleResult, report.microsoftResult].allSatisfy {
                    $0.state == .success || $0.state == .notCompleted
                }
                if healthyBase && !internet.hasCampusRecoveryEvidence {
                    await confirmRememberedCampusServiceIfNeeded(operation, report: report)
                }
                return
            }
        } else {
            if operation.kind == .automatic { policy.observeProbe(online: false) }
            if isOnline { record(SharedCopy.t("event.offlineAfterOnline")) }
            isOnline = false
            engineStatus = "offline"
            failedRound = true
        }
        guard mayAuthenticate && canAuthenticate else {
            if report.online { return }
            status = report.resultTitle
            detail = report.diagnosis.explanation
            authenticationSummary = SharedCopy.t("guard.detectionOnly")
            if operation.kind == .manual { record(SharedCopy.f("event.manualCheckFailed", ["reason": report.diagnosis.title])) }
            return
        }
        guard let credentials, hasCredentials else {
            engineStatus = "needsAccount"
            status = SharedCopy.t("status.needsAccount.headline")
            detail = operation.kind == .automatic ? SharedCopy.t("auto.needsAccount.detail") : SharedCopy.t("account.invalid")
            authenticationSummary = SharedCopy.t("guard.missingCredentials")
            return
        }
        let generation = credentialGeneration(for: credentials)
        let manualRound = operation.kind == .manual
        guard manualRound || admitAutomaticPhysicalRecovery() else { return }
        if !manualRound {
            if pausedForAutomatic {
                status = SharedCopy.t("status.paused.headline"); detail = strongestHoldMessage
                authenticationSummary = SharedCopy.t("status.paused.detail")
                engineStatus = "paused"
                return
            }
            if cooldownRemaining != nil {
                status = SharedCopy.t("status.coolingDown.headline"); detail = SharedCopy.t("status.coolingDown.detail")
                authenticationSummary = SharedCopy.t("auto.coolingDown.detail")
                engineStatus = "coolingDown"
                return
            }
            guard partialAutomatic || policy.consecutiveProbeFailures >= 2 else {
                let seconds = canPrioritizeRecovery ? 1 : 5
                status = SharedCopy.t("status.confirming.headline"); detail = report.diagnosis.explanation + SharedCopy.f("status.confirming.seconds", ["seconds": String(seconds)])
                authenticationSummary = SharedCopy.t("auto.ready.detail")
                engineStatus = "confirming"
                return
            }
        }
        partialRecoveryInProgress = partialAutomatic
        // AUTO-RECOVERY-01: both entry paths discover the campus first; a pinned manual area skips it.
        var discovery = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
        let discoveryChecked = !manualRound || authenticationArea == .automatic
        if discoveryChecked {
            if verificationFeedback?.serviceOnly == true { setVerificationFeedback(nil) }
            authenticationSummary = SharedCopy.t("status.discovering.headline")
            engineStatus = "discovering"
            // Preserve an HTTPS-only teaching memory when there is no contradictory
            // current hint. A fresh dormitory hint still gets its own service check.
            let hasDormitoryHint = report.appleResult.campusAreaHint == .dormitory
                || report.microsoftResult.campusAreaHint == .dormitory
            let discoveryArea: CampusArea = rememberedArea == .teaching && !hasDormitoryHint ? .teaching : authenticationArea
            lastDiscovery = nil
            confirmedCampusServiceContext = nil
            confirmedCampusServiceDeadline = nil
            discovery = await client.discoverCampus(area: discoveryArea)
            guard !Task.isCancelled, policy.owns(operation), checkRevision == networkRevision,
                  memoryGen == memoryGeneration else { return }
            campusServiceSelectionReport = report
            lastDiscovery = discovery
            if memoryVerdict(level: dormitoryMemoryLevel, discovery: discovery, ambiguous: memoryAmbiguous) == .authorized {
                touchMemory(area: discovery.dormitory == .confirmed ? .dormitory : .teaching)
            }
        }
        let planningChoice: CampusArea
        let allowUnsentFallback: Bool
        let routing = CampusRoutingDiagnostic(choice: authenticationArea, report: report, discovery: discovery,
            discoveryChecked: discoveryChecked, physicallyBound: requiresPhysicalBinding && authenticationRoute != nil,
            rememberedArea: rememberedCampusArea)
        lastRoutingDiagnostic = routing
        routingDiagnosticEpoch = networkEpoch
        record(SharedCopy.t("diagnostic.field.clues") + routing.summary)
        switch routing.resolution {
        case .selected(let choice, let fallback):
            planningChoice = choice
            allowUnsentFallback = fallback
            if discoveryChecked, discovery.confirmedCandidate(for: choice) != nil {
                markCampusServiceConfirmed()
                campusServiceRetry = nil
            }
        case .unconfirmed:
            status = SharedCopy.t("status.awaitingProof.headline")
            detail = SharedCopy.t("auto.awaitingProof.detail")
            authenticationSummary = SharedCopy.t("guard.noSubmission")
            engineStatus = "unconfirmed"
            return
        case .conflictingHints:
            status = SharedCopy.t("status.unconfirmed.headline")
            detail = SharedCopy.t("campus.entry.conflicting")
            authenticationSummary = SharedCopy.t("guard.noSubmission")
            engineStatus = "unconfirmed"
            return
        case .needsSelection:
            status = SharedCopy.t("status.unconfirmed.headline")
            detail = SharedCopy.t("campus.entry.bothDetected")
            authenticationSummary = SharedCopy.t("campus.entry.bothDetected")
            engineStatus = "unconfirmed"
            return
        }
        var target: CampusArea
        switch CampusPlan.decide(manual: manualRound, choice: planningChoice, discovery: discovery,
                                 trusted: policy.currentNetworkAuthorized, holds: policy.holds,
                                 generation: generation, epoch: networkEpoch, memory: dormitoryMemoryLevel, ambiguous: memoryAmbiguous) {
        case .submit(let area):
            target = area
        case .unconfirmed:
            status = manualRound ? SharedCopy.t("status.unconfirmed.headline") : SharedCopy.t("status.awaitingProof.headline")
            #if os(macOS)
            if tunnelActive {
                detail = SharedCopy.t("status.unconfirmed.physicalDetail")
            } else if systemProxyActive {
                detail = SharedCopy.t("status.unconfirmed.detail")
            } else {
                detail = SharedCopy.t("auto.awaitingProof.detail")
            }
            #else
            if tunnelActive || systemProxyActive {
                detail = SharedCopy.t("memory.wait.tunnel")
            } else {
                detail = SharedCopy.t("auto.awaitingProof.detail")
            }
            #endif
            authenticationSummary = SharedCopy.t("guard.noSubmission")
            engineStatus = "unconfirmed"
            return
        case .needsTrust:
            status = SharedCopy.t("status.awaitingProof.headline")
            detail = SharedCopy.t("auto.needsTrust.detail")
            authenticationSummary = SharedCopy.t("guard.noSubmission")
            engineStatus = "needsTrust"
            return
        case .paused:
            status = SharedCopy.t("status.paused.headline"); detail = strongestHoldMessage
            authenticationSummary = SharedCopy.t("status.paused.detail")
            engineStatus = "paused"
            return
        }
        guard !Task.isCancelled, policy.owns(operation) else { return }
        // A forget / revoke while this round was in flight must not be overtaken by its stale plan.
        guard memoryGen == memoryGeneration else { return }
        // Expiry can occur without a generation change while discovery is awaiting
        // its replies. Recheck physical permission immediately before submission.
        guard manualRound || admitAutomaticPhysicalRecovery() else { return }
        status = SharedCopy.t("status.loggingIn.headline"); detail = SharedCopy.t("status.loggingIn.detail")
        engineStatus = "loggingIn"
        let previousSubmissionCooldown = policy.cooldownState
        let previousSubmissionPending = pendingRecoverySubmission
        policy.authenticationAttemptStarted(now: now())
        pendingRecoverySubmission = true
        guard persistRecoveryJournal(requireWrite: true) else { refreshRecovery(); return }
        // Consume one recovery round, including an optional unsent-candidate fallback below.
        // An explicit manual retry remains available, but a persistent site fault never loops logins.
        partialRecovery.noteAttempt()
        credentialOperation = operation
        var attempt = await client.attemptLogin(credentials: credentials, area: target,
            requireBoundClientConfiguration: routing.source == .clientConfig)
        guard !Task.isCancelled, policy.owns(operation) else { return }
        diagnosticAttemptFinished = true
        recordAuthenticationDiagnostic(attempt, area: target, generation: generation)
        // Same round, still before any credential left the device: the teaching candidate was
        // unreachable or refused a challenge and the dormitory portal is confirmed → try it once.
        if attempt.submissionNotStarted,
           target == .teaching, authenticationArea == .automatic, allowUnsentFallback {
            let again = await client.discoverCampus(area: .dormitory)
            guard !Task.isCancelled, policy.owns(operation) else { return }
            let againRemembered = memoryVerdict(level: dormitoryMemoryLevel, discovery: again, ambiguous: memoryAmbiguous) == .authorized
            if memoryGen == memoryGeneration,
               (again.teaching == .confirmed && again.dormitory == .confirmed) || againRemembered,
               manualRound || policy.currentNetworkAuthorized || againRemembered,
               manualRound || policy.holds.blocking(.dormitory, generation: generation, epoch: networkEpoch) == nil {
                target = .dormitory
                attempt = await client.attemptLogin(credentials: credentials, area: target)
                guard !Task.isCancelled, policy.owns(operation) else { return }
                recordAuthenticationDiagnostic(attempt, area: target, generation: generation)
            }
        }
        if attempt.submissionNotStarted {
            // These classified results occur before the formal credential submission.
            // Preserve any older submitted request's deadline rather than extending it.
            policy.restoreCooldownState(previousSubmissionCooldown)
        }
        // Cancellation can arrive after a request reached the server. Keep the journal marker
        // until explicit resume, positive connectivity, or a definitive owned result.
        if attempt.submissionNotStarted {
            pendingRecoverySubmission = previousSubmissionPending
        } else if attempt.outcome != .cancelled { pendingRecoverySubmission = false }
        if prioritizedRound, !attempt.submissionNotStarted {
            rapidRecoveryDeadline = nil
            recoveryFullCheckPending = attempt.outcome == .success && probeMode == .complete
        }
        lastAttemptOutcome = attempt.outcome
        switch attempt.outcome {
        case .success:
            let released = policy.recordSuccess(area: target, generation: generation, epoch: networkEpoch)
            policy.authenticationAccepted(now: now(), clearBlock: false, scope: authenticationScope)
            touchMemory(area: target, authenticated: true)
            if manualRound, currentIdentity != nil, lastLinkActive {
                policy.authorizeAfterManualSuccess(operation)
                refreshAuthorization()
            }
            record(attempt.message ?? SharedCopy.t("event.accepted"))
            if !released.isEmpty { record(SharedCopy.f("event.holdReleased", ["area": target.title])) }
            refreshRecovery()
            // At most three read-only confirmations. Never submit credentials again here.
            engineStatus = "verifying"
            for delay in [0, 2, 5] {
                if delay > 0 { try? await verificationSleep(delay) }
                guard !Task.isCancelled, policy.owns(operation) else { return }
                let verificationReport = await runInternetCheck(mode: authenticationScope != nil ? .complete : selectedProbeMode, targets: selectedWebsites, strategy: selectedWebsiteStrategy,
                    deferWebsites: requiresPhysicalBinding && authenticationRoute != nil,
                    diagnosticContext: diagnosticRound.sample, diagnosticInterface: diagnosticRound.interface)
                guard !Task.isCancelled, policy.owns(operation), checkRevision == networkRevision else { return }
                let verification = verificationReport.base
                diagnosticLatestReport = verificationReport
                acceptReport(verification, internet: verificationReport)
                if verification.online {
                    partialRecovery.observe(partial: verificationReport.hasCampusRecoveryEvidence,
                        transportRecovered: verificationReport.transportRecovered || (quickRecovery && verificationReport.completeBasePassed))
                    policy.observeProbe(online: true)
                    failedRound = false
                    setOnline()
                    if (verificationReport.websitesDeferred && verificationReport.completeBasePassed) || verificationReport.verdict != .partial {
                        if verificationReport.websitesDeferred && probeMode == .complete { recoveryFullCheckPending = true }
                        return
                    }
                }
            }
            if lastInternetCheck?.base.online == true {
                setOnline()
                authenticationSummary = lastInternetCheck?.hasOnlyWebsiteIssues == true
                    ? SharedCopy.t("internet.websiteIssues.detail")
                    : SharedCopy.t("internet.partial.afterAttempt")
                return
            }
            isOnline = false; status = SharedCopy.t("status.accepted.headline")
            detail = SharedCopy.t("status.accepted.detail")
            authenticationSummary = SharedCopy.t("status.accepted.headline")
            engineStatus = "accepted"
            record(SharedCopy.t("event.accepted"))
        case .cancelled:
            preservePendingSubmission()
            failedRound = false
        default:
            isOnline = report.online
            status = SharedCopy.t("status.failed.headline")
            // Never expose an arbitrary transport error carrying a credential-bearing URL.
            detail = attempt.message ?? SharedCopy.t("portalError.network")
            engineStatus = attempt.outcome.engineStatus
            let hold = policy.recordFailure(automaticChoice: automaticAreaChoice, area: target,
                                            generation: generation, epoch: networkEpoch, attempt: attempt)
            record(detail)
            if let hold, hold.reason == .credentials, let heldArea = hold.area {
                record(SharedCopy.f("event.holdCreated", ["area": heldArea.title, "scope": SharedCopy.t(hold.sticky ? "hold.scope.sticky" : "hold.scope.network")]))
            }
            refreshRecovery()
        }
    }
    private func acceptReport(_ report: ConnectivityReport, internet: InternetCheckReport? = nil) {
        guard !isMaintaining else { return }
        lastInternetCheck = internet
        observeDirectNetworkResult(base: report)
        websiteConfigurationChanged = false
        if !report.online { offerForcedLogin = false }
        probeSummary = report.summary
        lastCheckedAt = internet?.completedAt ?? now()
        lastDiagnosis = report.diagnosis
        lastProbeMode = report.mode
        lastProbePassedCount = report.passedCount
        let sample = ConnectionSample(report: report, interface: interfaceSummary, date: lastCheckedAt ?? now())
        lastLatencyMilliseconds = sample.latencyMilliseconds
    }

    private func setOnline() {
        guard !isMaintaining else { return }
        let completelyPassed = requiresPhysicalBinding && authenticationRoute != nil
            ? lastInternetCheck?.completeBasePassed == true : lastInternetCheck?.verdict == .allPassed
        if completelyPassed {
            pendingRecoverySubmission = false
            policy.verifyAcceptedScope(authenticationScope, now: now())
        }
        if !isOnline { record(SharedCopy.t("event.online")) }
        // One reachable endpoint, including a quick check, cannot settle an unknown submission.
        if completelyPassed, policy.settleUncertainAuthentication() {
            record(SharedCopy.t("event.uncertainSettled"))
        }
        authenticationSummary = SharedCopy.t("diagnosis.online.title")
        engineStatus = "online"
        isOnline = true
        policy.resetFailures()
        if let internet = lastInternetCheck, internet.base.mode == .complete {
            if internet.websitesDeferred {
                status = SharedCopy.t("diagnosis.online.title")
                detail = SharedCopy.t("internet.baseRecovered")
            } else if internet.hasOnlyWebsiteIssues {
                status = SharedCopy.t("internet.websiteIssues.title")
                detail = SharedCopy.t("internet.websiteIssues.detail")
                authenticationSummary = detail
            } else {
                status = internet.verdict == .allPassed ? SharedCopy.t("internet.allPassed.title") : SharedCopy.t("dashboard.partialWithBase")
                detail = internet.verdict == .allPassed ? SharedCopy.t("internet.allPassed.detail")
                    : SharedCopy.t("internet.partial.detail")
            }
        } else {
            status = SharedCopy.t("env.internet.quickPassed")
            detail = SharedCopy.t("internet.quickDetail")
        }
    }
    @discardableResult
    private func persistRecoveryJournal(requireWrite: Bool = false) -> Bool {
        guard !isMaintaining else { return true }
        guard !sessionReadOnly else { return true }
        guard !isolated || recoveryJournalURL != nil else { return true }
        guard !recoveryJournalBlocked, let url = recoveryJournalURL else {
            policy.suspendAuthentication(.security)
            errorMessage = SharedCopy.t("error.recoveryJournalUnavailable")
            return false
        }
        let journal = RecoveryJournal(generation: savedGeneration, holds: policy.holds.holds,
            notBefore: policy.authenticationNotBefore, pendingSubmission: pendingRecoverySubmission)
        // A credential submission always verifies a durable marker; UI and
        // monitoring refreshes can skip an unchanged successful checkpoint.
        guard requireWrite || journal != lastRecoveryJournal else { return true }
        do {
            try journal.write(url)
            lastRecoveryJournal = journal
            return true
        } catch {
            lastRecoveryJournal = nil
            policy.suspendAuthentication(.security)
            errorMessage = SharedCopy.t("error.recoveryJournalWriteFailed")
            return false
        }
    }
    private func admitAutomaticPhysicalRecovery() -> Bool {
        guard requiresPhysicalBinding else { return true }
        guard authenticationRoute != nil, networkRemembered || policy.currentNetworkAuthorized else {
            engineStatus = authenticationRoute == nil ? "unconfirmed" : "needsTrust"
            status = SharedCopy.t("memory.remember")
            authenticationSummary = SharedCopy.t("memory.notRemembered")
            return false
        }
        return true
    }

    private func refreshRecovery() {
        guard !isMaintaining else { return }
        persistRecoveryJournal()
        automaticRecoveryPaused = pausedForAutomatic
        holds = policy.holds.holds
        updateWiFiRecoveryContext()
        if isPreview { recoverySummary = "离线演示，不会在后台检测"; return }
        let next = nextCheckAt.map { " · 下次 " + $0.formatted(date: .omitted, time: .standard) } ?? ""
        if sessionReadOnly { recoverySummary = "本次启动仅检测，不会登录" }
        else if sleeping { recoverySummary = SharedCopy.t("status.sleeping.detail") }
        else if monitoringMode == .observe { recoverySummary = SharedCopy.t("mode.observe.detail") + next }
        else if !autoReconnect { recoverySummary = SharedCopy.t("mode.manual.detail") }
        else if pausedForAutomatic { recoverySummary = strongestHoldMessage }
        else if !policy.available { recoverySummary = SharedCopy.t("status.noNetwork.detail") }
        else if automaticCredentials == nil { recoverySummary = SharedCopy.t("auto.needsAccount.detail") }
        else if isOnline { recoverySummary = SharedCopy.t("auto.onlineDetail") + next }
        else if cooldownRemaining != nil { recoverySummary = SharedCopy.t("auto.coolingDown.detail") }
        else if nextCheckAt != nil { recoverySummary = SharedCopy.t("auto.ready.detail") + next }
        else { recoverySummary = SharedCopy.t(busy ? "status.checking.headline" : "status.idle.headline") }
    }
    var recommendedCheckDelay: Int {
        if monitoringMode == .recover, !sessionReadOnly, probeMode == .quick,
           let deadline = sessionAuditDeadline, monotonicNow() >= deadline,
           partialRecovery.needsTransportConfirmation, policy.canRunAutomatic, !sleeping, !pausedForAutomatic { return 2 }
        if let deadline = rapidRecoveryDeadline, now() < deadline, engineStatus == "idle" || engineStatus == "noNetwork",
           policy.canRunAutomatic, !sleeping { return 0 }
        if recoveryFullCheckPending, probeMode == .complete, monitoringMode == .recover, !sessionReadOnly, policy.canRunAutomatic, !sleeping,
           !pausedForAutomatic, !pendingRecoverySubmission { return 2 }
        if canPrioritizeRecovery, engineStatus != "needsTrust" {
            return policy.consecutiveProbeFailures == 1 ? 1 : 3
        }
        if monitoringMode == .recover, !sessionReadOnly, hasSavedCredentials, !pausedForAutomatic,
           cooldownRemaining == nil, partialRecovery.needsConfirmation { return 5 }
        return policy.nextCheckDelay(constrained: constrained, now: now(), paused: pausedForAutomatic)
    }

    private var canPrioritizeRecovery: Bool {
        guard let deadline = rapidRecoveryDeadline, now() < deadline else { return false }
        return monitoringMode == .recover && !sessionReadOnly && hasSavedCredentials && policy.canRunAutomatic
            && !sleeping && !pausedForAutomatic && !pendingRecoverySubmission && cooldownRemaining == nil
    }
    var prioritizingCampusRecovery: Bool {
        (canPrioritizeRecovery && engineStatus != "needsTrust") || (recoveryFullCheckPending && probeMode == .complete)
    }
    private func clearPriorityRecovery() {
        rapidRecoveryDeadline = nil
        recoveryFullCheckPending = false
        sessionAuditDeadline = nil
    }

    var currentHealth: ConnectionHealth { health.snapshot(now: now()) }
    var diagnosticExportData: Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return (try? encoder.encode(diagnosticReport())) ?? Data("{}".utf8)
    }
    var diagnosticSummary: String { diagnosticReport().summary }

    private func recordAuthenticationDiagnostic(_ attempt: LoginAttempt, area: CampusArea, generation: UInt64?) {
        guard let diagnostic = attempt.diagnostic else { return }
        // Only typed, bounded evidence reaches history or diagnostic export. Never include
        // response text, a full URL, credentials, client IP, challenge or signed payload.
        let fields = [
            "area=\(area.rawValue)", "stage=\(diagnostic.stage.rawValue)", "path=\(diagnostic.path.rawValue)",
            "http=\(diagnostic.httpStatus.map(String.init) ?? "-")", "ac_id=\(diagnostic.acID ?? "-")",
            "result=\(attempt.outcome.rawValue)", "code=\(diagnostic.code.rawValue)",
            "server=\(diagnostic.serverCode?.rawValue ?? "-")", "saved=\(generation == nil ? "no" : "yes")"
        ]
        lastAuthDiagnosticSummary = fields.joined(separator: " ")
        let authentication = DiagnosticAuthentication(area: area.rawValue, stage: diagnostic.stage.rawValue, path: diagnostic.path.rawValue,
            httpStatus: diagnostic.httpStatus, acID: diagnostic.acID, result: attempt.outcome.rawValue,
            code: diagnostic.code.rawValue, serverCode: diagnostic.serverCode?.rawValue, saved: generation != nil)
        record(SharedCopy.t("diagnostic.field.authenticationEvent") + fields.joined(separator: " "), authentication: authentication, deduplicate: false)
    }
    private func record(_ message: String, context: DiagnosticFailureContext? = nil, authentication: DiagnosticAuthentication? = nil, date: Date? = nil, deduplicate: Bool = true) {
        guard !isMaintaining else { return }
        let now = date ?? self.now()
        // Path flapping and repetitive failures should not overwrite useful history.
        if deduplicate, context == nil, authentication == nil, let recent = events.first(where: { $0.message == message }), now.timeIntervalSince(recent.date) < 300 { return }
        events.insert(ConnectionEvent(date: now, message: message, context: context, authentication: authentication), at: 0)
        events = Array(events.prefix(300))
        guard !sessionReadOnly, let logURL else { return }
        do {
            try JSONEncoder().encode(events).write(to: logURL, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: logURL.path)
            #if os(iOS)
            try FileManager.default.setAttributes([.protectionKey: FileProtectionType.complete], ofItemAtPath: logURL.path)
            #endif
        } catch { errorMessage = SharedCopy.t("error.writeFailed") }
    }
}


// Diagnostic projections never feed authentication, scheduling or network selection.
private extension AppModel {
    struct DiagnosticRound {
        let sample: DiagnosticSampleContext
        let interface: String
        let cancellation: UInt64
        let discoveryRevision: UInt64
        let generation: UInt64?
        let epoch: UInt64
        let scope: PhysicalAuthenticationScope?
    }
    var diagnosticEnvironment: DiagnosticEnvironment {
        guard diagnosticFreshness.current, !sleeping, !isMaintaining, !networkInformationRefreshing, manualSnapshotID == nil else { return .unknown }
        let connection: DiagnosticEnvironment.Connection
        if !networkAvailable && !hasPhysicalConnection { connection = .none }
        else if connectedPhysicalLabels.count > 1 || currentDescriptors.count > 1 { connection = .multiple }
        else if authenticationRoute?.descriptor?.kind == "wifi" || interfaceSummary.contains("Wi-Fi") { connection = .wifi }
        else if authenticationRoute?.descriptor?.kind == "ethernet" || interfaceSummary.contains("有线") { connection = .ethernet }
        else { connection = tunnelActive ? .tunnelHidden : .unknown }
        return .init(connection: connection,
            systemProxy: diagnosticProxyKnown ? (systemProxyActive ? .on : .off) : .unknown,
            tun: tunnelActive ? .detected : .notDetected, refreshing: false)
    }
    func diagnosticRecovery(area originalArea: CampusArea, generation: UInt64?, epoch: UInt64,
                            scope: PhysicalAuthenticationScope?, environment: DiagnosticEnvironment) -> DiagnosticRecovery {
        let applicable = policy.holds.holds.filter { hold in
            originalArea == .automatic
                ? hold.applies(to: .teaching, generation: generation, epoch: epoch) || hold.applies(to: .dormitory, generation: generation, epoch: epoch)
                : hold.applies(to: originalArea, generation: generation, epoch: epoch)
        }
        let reasons = DiagnosticRecovery.Reason.allCases.filter { reason in applicable.contains { $0.reason.key == reason.rawValue } }
        let paused: Bool
        if monitoringMode != .recover || generation == nil { paused = false }
        else if originalArea == .automatic {
            paused = policy.holds.blocking(.teaching, generation: generation, epoch: epoch) != nil
                && policy.holds.blocking(.dormitory, generation: generation, epoch: epoch) != nil
        } else { paused = policy.holds.blocking(originalArea, generation: generation, epoch: epoch) != nil }
        let cooldown = DiagnosticRecovery.seconds(policy.cooldownRemaining(now: now(), scope: scope))
        let fresh = !environment.refreshing
        let state: DiagnosticRecovery.State
        if monitoringMode != .recover { state = .off }
        else if generation == nil { state = .needsAccount }
        else if paused { state = .paused }
        else if cooldown != nil { state = .coolingDown }
        else if !fresh { state = .unknown }
        else if !currentNetworkAuthorized { state = .needsTrust }
        else { state = requiredCampusVerificationState == "confirmed" ? .ready : .awaitingProof }
        return .init(state: state, readOnly: sessionReadOnly, paused: paused,
            networkAvailable: fresh && networkAvailable, networkRemembered: fresh && networkRemembered,
            sessionTrusted: fresh && currentNetworkAuthorized, holdReasons: reasons, cooldownRemainingSeconds: cooldown)
    }
    var diagnosticPhysical: DiagnosticPhysical? {
        guard let value = anonymousPhysicalDiagnostics else { return nil }
        return .init(physicalInterfaceCount: value.physicalInterfaceCount, serviceCount: value.serviceCount,
                     candidateCount: value.candidateCount, rejections: value.rejections)
    }
    func diagnosticRoundContext(operation: ConnectionPolicy.Operation) -> DiagnosticRound {
        let credentials = operation.kind == .automatic ? automaticCredentials : Credentials(account: account.trimmingCharacters(in: .whitespacesAndNewlines), password: password)
        return .init(sample: .init(operation: operation.kind == .manual ? "manual" : "automatic",
            monitoringMode: monitoringMode.rawValue, area: area.rawValue, startedAt: now(), environment: diagnosticEnvironment),
            interface: ConnectionSample.anonymousInterfaceLabel(interfaceSummary), cancellation: diagnosticCancellation,
            discoveryRevision: diagnosticDiscoveryRevision, generation: credentials.flatMap { credentialGeneration(for: $0) },
            epoch: networkEpoch, scope: authenticationScope)
    }
    func recordFailureContext(_ round: DiagnosticRound, kind: String, result: String) {
        guard round.cancellation == diagnosticCancellation, !sleeping, !isMaintaining,
              round.sample.area == area.rawValue, round.sample.monitoringMode == monitoringMode.rawValue else { return }
        let environment = diagnosticEnvironment
        let campus = diagnosticDiscoveryRevision != round.discoveryRevision && lastDiscovery != nil
            ? DiagnosticCampus(state: requiredCampusVerificationState, issue: lastDiscovery?.issue.rawValue ?? "none") : .notChecked
        let context = DiagnosticFailureContext(kind: kind, result: result, startedAt: round.sample.startedAt,
            collectedAt: now(), operation: round.sample.operation, monitoringMode: round.sample.monitoringMode,
            area: round.sample.area, environment: environment,
            status: DiagnosticFailureContext.statuses.contains(engineStatus) ? engineStatus : "unknown",
            campusVerification: environment.refreshing ? .notChecked : campus,
            recovery: diagnosticRecovery(area: area, generation: round.generation, epoch: round.epoch, scope: round.scope, environment: environment),
            physicalNetwork: environment.refreshing ? nil : diagnosticPhysical,
            physicalCollectedAt: environment.refreshing ? nil : physicalCollectedAt)
        guard context.valid else { return }
        record(SharedCopy.t("diagnostic.failureEvent"), context: context, date: context.collectedAt, deduplicate: false)
    }
    func appendHistoricalReport(_ report: InternetCheckReport, context: DiagnosticSampleContext?, interface: String) {
        guard !isMaintaining else { return }
        let sample = ConnectionSample(report: report.base, interface: interface, date: report.completedAt,
            websiteTotal: report.base.mode == .complete ? report.includedCount : nil,
            websitePassed: report.base.mode == .complete ? report.websitePassedCount : nil,
            context: context, completeResult: report.verdict.rawValue)
        health.append(sample, now: now())
        guard !sessionReadOnly, let healthURL else { return }
        do {
            try JSONEncoder().encode(health).write(to: healthURL, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: healthURL.path)
            #if os(iOS)
            try FileManager.default.setAttributes([.protectionKey: FileProtectionType.complete], ofItemAtPath: healthURL.path)
            #endif
        } catch { errorMessage = SharedCopy.t("error.writeFailed") }
    }
    struct DiagnosticSettings: Encodable {
        let monitoringMode: String
        let area: String
        let probeMode: String
        let websiteStrategy: String
    }
    struct DiagnosticReport: Encodable {
        let formatVersion = 1
        let exportedAt: Date
        let appVersion: String
        let build: String
        let mode: String
        let summary: String
        let measurement: String
        let samples: [ConnectionSample]
        let physicalNetwork: DiagnosticPhysical?
        let physicalCollectedAt: Date?
        let campusVerification: DiagnosticCampus
        let environment: DiagnosticEnvironment
        let recovery: DiagnosticRecovery
        let settings: DiagnosticSettings
        let events: [ConnectionEvent]
    }
    func diagnosticReport() -> DiagnosticReport {
        let environment = diagnosticEnvironment
        let recovery = diagnosticRecovery(area: area, generation: automaticCredentials == nil ? nil : savedGeneration,
                                          epoch: networkEpoch, scope: authenticationScope, environment: environment)
        let campus = environment.refreshing ? DiagnosticCampus.notChecked : DiagnosticCampus(
            state: campusVerificationInProgress ? "checking" : requiredCampusVerificationState,
            issue: lastDiscovery?.issue.rawValue ?? "none")
        let physical = environment.refreshing ? nil : diagnosticPhysical
        let samples = currentHealth.samples.map(\.anonymizedForExport)
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0.0"
        let unknown = SharedCopy.t("diagnostic.updating")
        let connectionText: String
        switch environment.connection {
        case .wifi: connectionText = "Wi-Fi"
        case .ethernet: connectionText = SharedCopy.t("env.access.ethernet")
        case .multiple: connectionText = SharedCopy.t("diagnostic.multiple")
        case .none: connectionText = SharedCopy.t("status.noNetwork.headline")
        case .tunnelHidden: connectionText = SharedCopy.t("env.tunnel.detected")
        case .unknown: connectionText = unknown
        }
        var lines = [SharedCopy.t("app.name") + " " + version,
            SharedCopy.t("diagnostic.field.state") + (environment.refreshing ? unknown : (websiteConfigurationChanged ? SharedCopy.t("internet.pending.title") : status)),
            SharedCopy.t("diagnostic.field.interface") + connectionText,
            "environment connection=\(environment.connection.rawValue) proxy=\(environment.systemProxy.rawValue) tun=\(environment.tun.rawValue) refreshing=\(environment.refreshing)",
            SharedCopy.t("diagnostic.field.websiteStrategy") + customWebsiteStrategy.title,
            SharedCopy.t("diagnostic.field.results") + (environment.refreshing ? unknown : probeSummary),
            SharedCopy.t("diagnostic.field.mode") + monitoringMode.title,
            SharedCopy.t("records.passedCount").replacingOccurrences(of: "{passed}", with: String(samples.filter(\.online).count)).replacingOccurrences(of: "{total}", with: String(samples.count)),
            SharedCopy.t("diagnostic.field.websites") + (environment.refreshing ? unknown : (lastInternetCheck.map { $0.base.mode == .complete ? SharedCopy.f("diagnostic.websiteCount", ["passed": String($0.websitePassedCount), "total": String($0.includedCount)]) : SharedCopy.t("internet.quickSkips") } ?? SharedCopy.t("connection.neverChecked"))),
            "recovery state=\(recovery.state.rawValue) readOnly=\(recovery.readOnly) paused=\(recovery.paused) available=\(recovery.networkAvailable) remembered=\(recovery.networkRemembered) trusted=\(recovery.sessionTrusted)",
            "protection reasons=\(recovery.holdReasons.map(\.rawValue).joined(separator: ",")) cooldownSeconds=\(recovery.cooldownRemainingSeconds.map(String.init) ?? "-")",
            SharedCopy.t("diagnostic.field.checkedAt") + (environment.refreshing ? unknown : (lastCheckedAt?.formatted(date: .numeric, time: .standard) ?? SharedCopy.t("connection.neverChecked"))),
            "campus state=\(campus.state) issue=\(campus.issue)"]
        #if os(macOS)
        lines.append(wifiRecovery.diagnosticSummary)
        #endif
        if campus.issue != "none" { lines.append(SharedCopy.t("diagnostic.field.clues") + SharedCopy.t("campus.verify." + campus.issue + "Hint")) }
        if let lastAuthDiagnosticSummary { lines.append(SharedCopy.t("diagnostic.field.authenticationLast") + lastAuthDiagnosticSummary) }
        if !environment.refreshing, let lastRoutingDiagnostic { lines.append(SharedCopy.t("diagnostic.field.clues") + lastRoutingDiagnostic.summary) }
        if let physical {
            lines.append(SharedCopy.f("env.identification.summary", ["reason": physicalIdentificationDetail]))
            let reasons = physical.rejections.keys.sorted().map { "\($0)=\(physical.rejections[$0]!)" }.joined(separator: ",")
            lines.append("physical interfaces=\(physical.physicalInterfaceCount) services=\(physical.serviceCount) candidates=\(physical.candidateCount) reasons=\(reasons.isEmpty ? "none" : reasons)")
        }
        let failures = samples.filter(\.isHistoricalFailure).suffix(3).reversed()
        if !failures.isEmpty { lines.append(SharedCopy.t("diagnostic.recentFailures")) }
        for sample in failures {
            let appleTime = sample.appleMilliseconds.map { " \(Int($0.rounded())) ms" } ?? ""
            let microsoftTime = sample.microsoftMilliseconds.map { " \(Int($0.rounded())) ms" } ?? ""
            lines.append("\(sample.date.formatted(date: .numeric, time: .standard)) · \((sample.completeResult == "partial" ? "partial" : sample.diagnosis.rawValue)) · Apple \(sample.appleState)\(appleTime) · Microsoft \(sample.microsoftState)\(microsoftTime)")
        }
        let allowed = Set(SharedCopy.table.values)
        let safeEvents = events.prefix(300).map { event in
            ConnectionEvent(id: event.id, date: event.date,
                message: event.context != nil ? SharedCopy.t("diagnostic.failureEvent")
                    : event.authentication != nil ? SharedCopy.t("diagnostic.field.authenticationEvent")
                    : allowed.contains(event.message) ? event.message : SharedCopy.t("diagnostic.legacyEvent"), context: event.context, authentication: event.authentication)
        }
        return .init(exportedAt: now(), appVersion: version,
            build: Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "34", mode: monitoringMode.rawValue,
            summary: lines.joined(separator: "\n"), measurement: SharedCopy.t("records.measurement"), samples: samples,
            physicalNetwork: physical, physicalCollectedAt: physical == nil ? nil : physicalCollectedAt,
            campusVerification: campus, environment: environment, recovery: recovery,
            settings: .init(monitoringMode: monitoringMode.rawValue, area: area.rawValue, probeMode: probeMode.rawValue,
                            websiteStrategy: customWebsiteStrategy.rawValue), events: safeEvents)
    }
}

/// This transport is selected before an offline preview can perform any UI action.
actor PreviewConnectionService: ConnectionService {
    private let partialPreview = Bundle.main.object(forInfoDictionaryKey: "SZUPreviewPartial") as? Bool == true
    private let teachingOnly = Bundle.main.object(forInfoDictionaryKey: "SZUPreviewTeachingOnly") as? Bool == true
    private var completedPreviewLogin = false
    func probe() async -> ConnectivityReport { await probe(mode: .quick) }
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        // A visible, cancellable busy interval lets the isolated UI exercise concurrent actions.
        try? await Task.sleep(for: .seconds(3))
        if Task.isCancelled {
            return ConnectivityReport(appleResult: .init(state: .cancelled, elapsed: 0),
                                      microsoftResult: .init(state: .cancelled, elapsed: 0), mode: mode)
        }
        return ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.024),
                           microsoftResult: .init(state: mode == .complete
                                ? (partialPreview && !completedPreviewLogin ? .redirect : .success) : .notCompleted,
                                                  elapsed: mode == .complete ? 0.041 : 0), mode: mode)
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        try? await Task.sleep(for: .seconds(2))
        guard !Task.isCancelled else { return CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked) }
        return CampusDiscovery(teaching: .confirmed, dormitory: teachingOnly ? .unconfirmed : .confirmed)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { nil }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        guard partialPreview else { return LoginAttempt(outcome: .security) }
        try? await Task.sleep(for: .seconds(1))
        guard !Task.isCancelled else { return LoginAttempt(outcome: .cancelled) }
        completedPreviewLogin = true
        return LoginAttempt(outcome: .success, message: "离线演示：已模拟登录，没有访问真实网络。")
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        throw PortalError.forbiddenTarget
    }
}
