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
}
extension PortalClient: ConnectionService {}

/// Read-only facts about the user's own network setup. The app never uses or changes them;
/// URLSession ignores these proxy settings; packet-tunnel routing remains system-managed.
enum NetworkEnvironment {
    static func systemProxyEnabled() -> Bool {
        guard let unmanaged = CFNetworkCopySystemProxySettings(),
              let settings = unmanaged.takeRetainedValue() as NSDictionary as? [String: Any] else { return false }
        return ["HTTPEnable", "HTTPSEnable", "SOCKSEnable", "ProxyAutoConfigEnable"].contains { key in
            (settings[key] as? NSNumber)?.boolValue == true
        }
    }
}

private let defaultTrustReason = SharedCopy.t("guard.trustDetail")

@MainActor final class AppModel: ObservableObject {
    @Published var account = "" { didSet { if account != oldValue { configurationChanged() } } }
    @Published var password = "" { didSet { if password != oldValue { configurationChanged() } } }
    @Published var area: CampusArea = .automatic {
        didSet {
            guard area != oldValue else { return }
            defaults?.set(area.rawValue, forKey: "area")
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
    @Published private(set) var lastInternetCheck: InternetCheckReport?
    @Published private(set) var systemConnectivity: ConnectivityReport?
    @Published private(set) var systemConnectivityChecking = false
    @Published private(set) var systemConnectivityCheckedAt: Date?
    @Published private(set) var networkInformationRefreshing = false
    @Published private(set) var networkInformationFeedback: String?
    private var networkInformationTask: Task<Void, Never>?
    private let networkInformationReader: (@Sendable () async throws -> PathObservation)?
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
    private let client: any ConnectionService
    private let defaults: UserDefaults?
    private let isolated: Bool
    private let automaticLoopAllowed: Bool
    private let loopSleep: @Sendable (Int) async throws -> Void
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
    private var loopID: UUID?
    private var manual: Task<Void, Never>?
    private var probeOwner: ConnectionPolicy.Operation?
    @Published private var activeInternetCheck: UUID?
    var physicalConnectivityChecking: Bool { activeInternetCheck != nil }
    private var logURL: URL?
    private var healthURL: URL?
    private var recoveryJournalURL: URL?
    private var lastRecoveryJournalData: Data?
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
    private var authorizationGraceDeadline: Date?
    private var authorizationGraceTask: Task<Void, Never>?
    // Authentication holds are scoped by saved-credential generation and network epoch.
    private var savedGeneration: UInt64 = 1
    private var networkEpoch: UInt64 = 1
    private var websiteEnvironmentRevision: UInt64 = 0
    private var websitePathIdentity: NetworkIdentity?
    @Published private(set) var holds: [AuthenticationHold] = []
    @Published private(set) var lastDiscovery: CampusDiscovery?
    @Published private(set) var lastAttemptOutcome: LoginOutcome?
    /// Shared status vocabulary (shared/status-fixtures.json) consumed by StatusPresenter.
    @Published private(set) var engineStatus = "idle"
    // Network memory stores keyed hashes of stable features, never raw values.
    @Published private(set) var rememberedNetworks: [MemoryRecord] = []
    @Published private(set) var memoryUnreadable = false
    @Published private(set) var memoryWriteFailed = false
    @Published private(set) var memoryVerificationMessage: String?
    @Published private(set) var isVerifyingMemory = false
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
         now: @escaping () -> Date = Date.init, recoveryStore: URL? = nil, isolatedCredentials: Credentials? = nil,
         websiteChecker: (any WebsiteChecking)? = nil, websiteStore: URL? = nil,
         initialWebsiteTargets: [WebsiteTarget] = [], memoryStore: URL? = nil,
         runAutomaticLoopInIsolation: Bool = false, loopSleep: (@Sendable (Int) async throws -> Void)? = nil,
         labelsStore: URL? = nil, isolatedPreferences: UserDefaults? = nil, recordStoreDirectory: URL? = nil,
         isolatedWiFiRecoveryStore: URL? = nil,
         systemConnectivityProbe: (@Sendable (ConnectivityProbeMode) async -> ConnectivityReport)? = nil,
         verificationSleep: (@Sendable (Int) async throws -> Void)? = nil,
         networkInformationReader: (@Sendable () async throws -> PathObservation)? = nil,
         credentialDeletion: (@Sendable () async throws -> Void)? = nil) {
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
        self.isPreview = preview
        self.client = preview ? PreviewConnectionService() : client
        self.websiteChecker = preview ? PreviewWebsiteChecker() : (websiteChecker ?? (isolated ? UnconfiguredWebsiteChecker() : WebsiteProbe()))
        self.isolated = isolated || preview
        self.networkInformationReader = networkInformationReader
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
            Task { @MainActor [weak self] in await self?.refreshNetworkEnvironmentInBackground() }
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
            lastCheckedAt = Date()
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
        workspaceObservers.append(center.addObserver(forName: NSWorkspace.willSleepNotification,
                                                      object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.setSleeping(true) }
        })
        workspaceObservers.append(center.addObserver(forName: NSWorkspace.didWakeNotification,
                                                      object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.setSleeping(false) }
        })
        #endif
        systemProxyActive = NetworkEnvironment.systemProxyEnabled()
        let observer = NWPathMonitor()
        monitor = observer
        observer.pathUpdateHandler = { [weak self] path in
            // Captured on the monitor queue; only value types cross to the main actor.
            let observation = PathObservation.capture(path)
            Task { @MainActor [weak self] in self?.receive(observation) }
        }
        observer.start(queue: DispatchQueue(label: "local.SZUConnect.path"))
        #if os(macOS)
        // Route-only TUN changes may not change NWPath's default physical interface.
        // This refresh reads local metadata only; it sends no network probe or credential.
        environmentRefreshTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(5)) } catch { return }
                guard let self else { return }
                await self.refreshNetworkEnvironmentInBackground()
            }
        }
        #endif
    }

    deinit {
        networkInformationTask?.cancel()
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

    private func loadRecoveryJournal(_ url: URL) {
        recoveryJournalURL = url
        do {
            if let journal = try RecoveryJournal.read(url, now: now()) {
                savedGeneration = journal.generation
                policy.restoreRecovery(journal)
                lastRecoveryJournalData = try journal.encoded()
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

    private func receive(_ observation: PathObservation) {
        defer { refreshSystemConnectivityIfNeeded() }
        observeWebsitePath(observation)
        receiveResolved(resolveAuthenticationNetwork(observation))
    }

    private func observeWebsitePath(_ observation: PathObservation) {
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
        // A system-primary notification cannot move an in-flight credential operation
        // away from its still-valid, independently remembered physical attachment.
        let activeRoute = policy.active == nil ? nil : known.first { $0 == authenticationRoute }
        guard let route = activeRoute ?? known.first ?? candidates.first(where: \.isSystemPrimary), let descriptor = route.descriptor else {
            var continuity = observation.continuity
            continuity.authenticationRoute = nil
            continuity.requiresPhysicalBinding = true
            continuity.descriptors = []
            continuity.trustUnavailableReason = SharedCopy.t("env.identification.pending")
            return PathObservation(usable: observation.usable, interfaces: observation.interfaces,
                                   constrained: observation.constrained, continuity: continuity, physicalCandidates: observation.physicalCandidates)
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
                               continuity: continuity, physicalCandidates: observation.physicalCandidates)
    }

    private func receiveResolved(_ observation: PathObservation) {
        if !isolated { systemProxyActive = NetworkEnvironment.systemProxyEnabled() }
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
        #if os(macOS)
        guard !isolated, !sleeping, let path = monitor?.currentPath else { return }
        applyEnvironmentObservation(PathObservation.capture(path))
        #endif
    }

    private func refreshNetworkEnvironmentInBackground() async {
        #if os(macOS)
        guard !isolated, !sleeping, let path = monitor?.currentPath else { return }
        let revision = networkRevision
        let websiteRevision = websiteEnvironmentRevision
        let observation = await Task.detached(priority: .utility) { PathObservation.capture(path) }.value
        guard !Task.isCancelled, !sleeping, revision == networkRevision,
              websiteRevision == websiteEnvironmentRevision else { return }
        applyEnvironmentObservation(observation)
        #endif
    }

    func applyEnvironmentObservation(_ incoming: PathObservation) {
        defer { refreshSystemConnectivityIfNeeded() }
        observeWebsitePath(incoming)
        let observation = resolveAuthenticationNetwork(incoming)
        observeAttachedWiFiNames(observation.continuity)
        let proxy = isolated ? systemProxyActive : NetworkEnvironment.systemProxyEnabled()
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
        cancelOperations()
        if interruptedMemoryVerification {
            memoryVerificationMessage = SharedCopy.t(usable ? "memory.verification.changed" : "memory.verification.disconnected")
        }
        systemConnectivityRecheckNeeded = recheckSystem && usable && !sleeping && monitoringMode != .manual
        if !firstPath { invalidateTransport() }
        networkRevision &+= 1
        lastInternetCheck = nil
        websiteConfigurationChanged = false
        if !sameIdentity || authenticationTunnelChanged || descriptorsChanged || trustScopeChanged || bindingChanged {
            if !interruptedMemoryVerification { memoryVerificationMessage = nil }
            // A different network: holds scoped to the previous network no longer apply.
            networkEpoch &+= 1
            partialRecovery.reset()
            policy.pathChanged(to: networkEpoch)
            lastDiscovery = nil // evidence about the previous network says nothing about this one
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
        guard !isolated else { return }
        authorizationGraceTask?.cancel()
        authorizationGraceTask = Task { [weak self] in
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
        guard initialized, !credentialsDeletionInProgress else { return }
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
        account = automaticCredentials?.account ?? ""
        password = automaticCredentials?.password ?? ""
    }

    /// The caller presents confirmation. The exact application Keychain item is the only deletion target.
    func deleteSavedCredentials() async {
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
    var canAuthenticate: Bool { !sessionReadOnly && !credentialsDeletionInProgress && monitoringMode != .observe }
    /// Dormitory (or automatic) recovery needs an explicit trust decision for this network.
    var needsNetworkTrust: Bool { monitoringMode == .recover && area != .teaching }
    /// Work the user should see: any manual action, or anything while not yet online.
    /// Routine background checks of a healthy connection stay quiet.
    var isForegroundBusy: Bool { busy && (manual != nil || !isOnline || partialRecoveryInProgress) }

    func connect() {
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
    func connectEvenIfOnline() {
        guard canOfferForcedLogin else { return }
        startManual(mayAuthenticate: true, force: true)
    }
    func checkConnection() { startManual(mayAuthenticate: false) }

    /// No result here changes trust, holds, authentication, history or recovery policy.
    func refreshSystemConnectivity() {
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
        systemConnectivityTask = Task { [weak self] in
            let report = await probe(mode)
            guard let self, !Task.isCancelled, generation == self.systemConnectivityGeneration,
                  !self.sleeping, self.networkAvailable || self.hasPhysicalConnection else { return }
            self.systemConnectivity = report
            self.systemConnectivityCheckedAt = self.now()
            self.systemConnectivityChecking = false
            self.systemConnectivityTask = nil
        }
    }

    func refreshDisplayedNetwork() {
        guard !networkInformationRefreshing, !sleeping else { return }
        networkInformationRefreshing = true
        networkInformationFeedback = nil
        let revision = networkRevision
        networkInformationTask = Task { [weak self] in
            guard let self else { return }
            defer { self.networkInformationRefreshing = false; self.networkInformationTask = nil }
            do {
                let observation: PathObservation?
                if let reader = self.networkInformationReader { observation = try await reader() }
                else if self.isolated { observation = nil }
                else if let path = self.monitor?.currentPath {
                    observation = await Task.detached(priority: .utility) { PathObservation.capture(path) }.value
                } else { throw CancellationError() }
                guard !Task.isCancelled, !self.sleeping else { return }
                guard self.networkRevision == revision else {
                    self.networkInformationFeedback = SharedCopy.t("env.refreshChanged"); return
                }
                if let observation { self.applyEnvironmentObservation(observation) }
                let partial = self.hasPhysicalConnection && self.currentIdentity == nil
                self.networkInformationFeedback = SharedCopy.f(partial ? "env.refreshedPartial" : "env.refreshed",
                    ["time": self.now().formatted(date: .omitted, time: .standard)])
            } catch {
                if !Task.isCancelled { self.networkInformationFeedback = SharedCopy.t("env.refreshFailed") }
            }
        }
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
    var canEditWebsiteTargets: Bool { !sessionReadOnly && (!busy || probeOwner != nil) }

    @discardableResult
    func saveCustomWebsites(_ targets: [WebsiteTarget], preferComplete: Bool = true, strategy: WebsiteProbeStrategy? = nil) -> Bool {
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
        var targets = customWebsiteTargets
        guard let index = targets.firstIndex(where: { $0.id == id }) else { return }
        targets[index].enabled = enabled
        _ = saveCustomWebsites(targets, preferComplete: enabled)
    }
    func setWebsiteStrategy(_ strategy: WebsiteProbeStrategy) {
        _ = saveCustomWebsites(customWebsiteTargets, preferComplete: false, strategy: strategy)
    }
    func removeWebsite(_ id: String) { _ = saveCustomWebsites(customWebsiteTargets.filter { $0.id != id }, preferComplete: false) }
    @discardableResult func addWebsite(_ input: String) -> Bool {
        saveCustomWebsites(customWebsiteTargets + [WebsiteTarget(url: input)], preferComplete: true)
    }
    private func runInternetCheck(mode: ConnectivityProbeMode, targets: [WebsiteTarget], strategy: WebsiteProbeStrategy,
                                  prioritizeCampusRecovery: Bool = false, deferWebsites: Bool = false) async -> InternetCheckReport {
        let check = UUID()
        activeInternetCheck = check
        defer { if activeInternetCheck == check { activeInternetCheck = nil } }
        let websiteRevision = websiteEnvironmentRevision
        let report = await InternetChecker.check(mode: mode, targets: targets, checker: websiteChecker, strategy: strategy,
            prioritizeCampusRecovery: prioritizeCampusRecovery, deferWebsites: deferWebsites) { [client] selectedMode in
            await client.probe(mode: selectedMode)
        }
        guard websiteRevision == websiteEnvironmentRevision else {
            // TUN/proxy changes obsolete website observations, but not the bound campus probe.
            return InternetCheckReport(base: report.base, websites: [], selectedCount: report.selectedCount,
                wasCancelled: report.wasCancelled, websitesDeferred: true)
        }
        return report
    }

    func setProbeMode(_ mode: ConnectivityProbeMode) {
        guard probeMode != mode else { return }
        if mode == .quick { recoveryFullCheckPending = false }
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
    var memoryAmbiguous: Bool { tunnelActive || currentDescriptors.count > 1 }
    var physicalNetworkIdentified: Bool { currentIdentity != nil && lastLinkActive && (!requiresPhysicalBinding || authenticationRoute != nil) }
    var authenticationTargetName: String? {
        guard let route = authenticationRoute else { return nil }
        if networkRemembered { return networkLabels.displayName(for: memoryMatch.record) }
        return observedNetworkLabel?.displayName ?? (route.descriptor?.kind == "wifi" ? SharedCopy.t("memory.item.wifi") : SharedCopy.t("memory.item.ethernet"))
    }
    var trustUnavailableDetail: String {
        networkTrustUnavailableReason ?? SharedCopy.t("banner.networkUnidentified.detail")
    }
    var hasMultipleNetworkAttachments: Bool { connectedPhysicalLabels.count > 1 || currentDescriptors.count > 1 }
    var networkRemembered: Bool { memoryLevel == .medium || memoryLevel == .strong }
    /// Current-attachment service evidence; session trust and public connectivity never supply proof.
    var campusVerificationState: String {
        guard networkAvailable, !sleeping, let discovery = lastDiscovery else { return "notChecked" }
        let confirmed = discovery.teaching == .confirmed && (area != .dormitory || discovery.dormitory == .confirmed)
        if confirmed { return "confirmed" }
        return discovery.teaching == .notChecked && discovery.dormitory == .notChecked ? "notChecked" : "unconfirmed"
    }
    var memoryVerdictNow: MemoryVerdict {
        memoryVerdict(level: dormitoryMemoryLevel, discovery: lastDiscovery ?? CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked),
                      ambiguous: memoryAmbiguous)
    }
    /// "" when remembering is possible; otherwise needsProof | unreliable | readOnly | unreadable.
    var rememberBlocker: String {
        if sessionReadOnly { return "readOnly" }
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
        (!requiresPhysicalBinding || authenticationRoute != nil) && !sessionOnlyTrust && !sessionReadOnly && !memoryUnreadable && memory != nil && policy.available && policy.foreground
            && !sleeping && lastLinkActive && currentIdentity != nil
            && currentDescriptors.count == 1 && currentDescriptors[0].isReliable
            && !busy && policy.active == nil
    }

    /// An explicit, cancellable setup action. It probes fixed campus services without logging in.
    func verifyAndRememberCurrentNetwork() {
        refreshNetworkEnvironment()
        guard canVerifyAndRememberNetwork else {
            let reason = sessionReadOnly ? "readOnly" : memoryUnreadable ? "unreadable" : isVerifyingMemory ? "checking" : busy ? "busy"
                : sessionOnlyTrust || currentDescriptors.count > 1 ? "unreliable" : "unavailable"
            memoryVerificationMessage = SharedCopy.t("memory.verification." + reason)
            return
        }
        #if os(macOS)
        if let memory, let descriptor = currentDescriptors.first,
           !wifiRecovery.preserveExclusionsBeforeMerging(memory.mergeCandidates(descriptor, now: now())) {
            memoryVerificationMessage = SharedCopy.t("memory.verification.writeFailed")
            return
        }
        #endif
        stopLoop()
        guard let operation = policy.begin(.manual) else { return }
        let revision = networkRevision
        let generation = memoryGeneration
        let selectedArea = area
        let descriptors = currentDescriptors
        let identity = currentIdentity
        busy = true
        isVerifyingMemory = true
        memoryVerificationMessage = SharedCopy.t("memory.verification.checking")
        lastDiscovery = nil
        engineStatus = "discovering"
        status = SharedCopy.t("status.discovering.headline")
        authenticationSummary = SharedCopy.t("memory.verification.checking")
        manual = Task { [weak self] in
            guard let self else { return }
            defer {
                if self.policy.finish(operation) {
                    self.isVerifyingMemory = false
                    self.busy = false
                    self.manual = nil
                    self.engineStatus = self.isOnline ? "online" : "idle"
                    self.refreshRecovery()
                    self.startLoop(initialDelay: 2)
                }
            }
            guard await self.prepareTransport(for: operation) else {
                if self.policy.owns(operation) {
                    self.memoryVerificationMessage = SharedCopy.t("memory.verification.needsProof")
                }
                return
            }
            let discovered = await self.client.discoverCampus(area: selectedArea == .teaching ? .teaching : .automatic)
            guard !Task.isCancelled, self.policy.owns(operation), self.networkRevision == revision,
                  self.memoryGeneration == generation, self.currentIdentity == identity,
                  self.currentDescriptors == descriptors, self.area == selectedArea else { return }
            self.lastDiscovery = discovered
            let scope: CampusArea?
            if discovered.teaching != .confirmed { scope = nil }
            else if selectedArea == .teaching { scope = .teaching }
            else if discovered.dormitory == .confirmed { scope = .dormitory }
            else { scope = selectedArea == .automatic ? .teaching : nil }
            guard let scope else {
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.needsProof")
                self.authenticationSummary = SharedCopy.t("memory.verification.needsProof")
                return
            }
            switch self.rememberVerifiedNetwork(scope: scope) {
            case .remembered:
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.remembered")
            case .failed:
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.writeFailed")
            default:
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.pending")
            }
        }
    }

    /// Called only by the same-operation fresh verification above; disk commits before authorization.
    private func rememberVerifiedNetwork(scope: CampusArea) -> RememberResult {
        guard rememberBlocker.isEmpty, var next = memory, let descriptor = currentDescriptors.first else { return .needsProof }
        let dormitory = scope == .dormitory
        let rememberedAt = now()
        #if os(macOS)
        guard wifiRecovery.preserveExclusionsBeforeMerging(next.mergeCandidates(descriptor, now: rememberedAt)) else { return .failed }
        #endif
        guard let id = next.remember(descriptor, area: dormitory ? "dormitory" : "teaching",
                                     proofTeaching: true, proofDormitory: dormitory,
                                     now: rememberedAt, newID: NetworkMemory.randomHex()) else { return .unreliable }
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
        guard !sessionReadOnly, !memoryUnreadable, var next = memory,
              rememberedNetworks.contains(where: { $0.id == id }) else { return }
        let ids = next.equivalentIDs(id: id)
        let isCurrent = !Set(next.matchingIDs(currentDescriptors, now: now())).isDisjoint(with: ids)
        guard !ids.isEmpty else { return }
        for removed in ids { next.forget(id: removed) }
        if isCurrent {
            memoryVerificationMessage = nil
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
        guard !sessionReadOnly else { return }
        memoryVerificationMessage = nil
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
        guard !sessionReadOnly else { return }
        memoryVerificationMessage = nil
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
        observedNetworkLabel = label
        guard label != nil, currentIdentity != nil, lastLinkActive, policy.available,
              currentDescriptors.count == 1, currentDescriptors[0].isReliable,
              !sleeping, let memory, !memoryUnreadable else { return }
        let ids = memory.matchingIDs(currentDescriptors, now: now()).filter { !suppressedMemory.contains($0) }
        saveObservedNetworkLabel(ids: ids)
    }

    /// A connected secondary Wi-Fi can provide its own name; it never supplies egress or login authority.
    private func observeAttachedWiFiNames(_ snapshot: NetworkContinuitySnapshot) {
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

    func rememberedNetworkDisplayName(for id: String) -> String {
        let name = networkLabels.displayName(for: id)
        guard let index = networkLabels.sameNameIndex(for: id, among: rememberedNetworks.map(\.id)) else { return name }
        return SharedCopy.f("memory.sameName", ["name": name, "number": String(index)])
    }

    func rememberedNetworkDetail(for record: MemoryRecord) -> String {
        let kind = networkLabels.entries[record.id]?.kind.rawValue ?? "other"
        let connection = SharedCopy.t("memory.kind." + kind)
        guard record.area == "dormitory" || record.area == "teaching" else { return connection }
        return connection + " · " + SharedCopy.t("area." + record.area + ".title")
    }

    private func updateWiFiRecoveryContext() {
        #if os(macOS)
        guard initialized else { return }
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

    private func touchMemory(area: CampusArea) {
        guard var next = memory else { return }
        let found = area == .dormitory
            ? next.matchAny(currentDescriptors, now: now(), exclude: suppressedMemory, requireDormitoryProof: true)
            : memoryMatch
        if area == .teaching,
           next.records.contains(where: { $0.id == found.record && $0.proofDormitory }) { return }
        guard !found.record.isEmpty,
              next.touch(id: found.record, now: now(), area: area == .dormitory ? "dormitory" : "teaching") else { return }
        _ = commitMemory(next, suppress: [])
    }

    func authorizeCurrentNetwork() {
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
        cancelOperations()
        revokeAuthorization(reason: SharedCopy.t("event.trustRevoked"))
        authenticationSummary = SharedCopy.t("event.trustRevoked")
        startLoop()
    }
    func resumeAutomaticRecovery() {
        guard !sessionReadOnly, !credentialsDeletionInProgress, monitoringMode == .recover, automaticCredentials != nil else { return }
        cancelOperations()
        if recoveryJournalBlocked, let url = recoveryJournalURL {
            do {
                if FileManager.default.fileExists(atPath: url.path) { _ = try EventLogFile.quarantine(url) }
                recoveryJournalBlocked = false; lastRecoveryJournalData = nil
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
        (!requiresPhysicalBinding || authenticationRoute != nil) && !sessionReadOnly && policy.available && !sleeping && currentIdentity != nil && lastLinkActive
    }
    private func credentialGeneration(for credentials: Credentials) -> UInt64? {
        guard let saved = automaticCredentials, saved.account == credentials.account, saved.password == credentials.password else { return nil }
        return savedGeneration
    }

    func cancelCurrentOperation() {
        // Cancel this round only; automatic monitoring may resume after a quiet interval.
        if isVerifyingMemory { memoryVerificationMessage = SharedCopy.t("memory.verification.cancelled") }
        cancelOperations()
        status = isOnline ? SharedCopy.t("diagnosis.online.title") : SharedCopy.t("status.cancelled.headline")
        engineStatus = isOnline ? "online" : "cancelled"
        detail = SharedCopy.t("status.cancelled.detail")
        authenticationSummary = SharedCopy.t("status.cancelled.detail")
        startLoop(initialDelay: 30)
        refreshRecovery()
    }
    func setAutoReconnect(_ enabled: Bool) {
        setMonitoringMode(enabled ? .recover : .manual)
    }
    func setMonitoringMode(_ mode: MonitoringMode) {
        guard monitoringMode != mode else { return }
        if mode != .recover { clearPriorityRecovery() }
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
        guard sleeping != value else { return }
        let interruptedMemoryVerification = isVerifyingMemory
        sleeping = value
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
            memoryVerificationMessage = SharedCopy.t("memory.verification.sleeping")
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
            receive(PathObservation.capture(path))
            authorizationReason = SharedCopy.t("guard.afterWake")
        }
        refreshAuthorization()
        refreshRecovery()
    }
    private func stopLoop() {
        loop?.cancel(); loop = nil; loopID = nil; nextCheckAt = nil
    }
    private func startLoop(initialDelay: Int = 2) {
        guard !credentialsDeletionInProgress, automaticLoopAllowed, policy.canRunAutomatic, !sleeping, loop == nil else { return }
        let id = UUID()
        loopID = id
        nextCheckAt = now().addingTimeInterval(TimeInterval(initialDelay))
        let sleepLoop = loopSleep
        loop = Task { [weak self] in
            defer {
                if let self, self.loopID == id {
                    self.loop = nil; self.loopID = nil; self.nextCheckAt = nil
                }
            }
            do { try await sleepLoop(initialDelay) } catch { return }
            while !Task.isCancelled {
                guard let self, self.loopID == id, self.policy.canRunAutomatic, !self.sleeping else { return }
                self.nextCheckAt = nil
                await self.runAutomaticCycle()
                guard !Task.isCancelled, self.loopID == id else { return }
                let delay = self.recommendedCheckDelay
                self.nextCheckAt = self.now().addingTimeInterval(TimeInterval(delay))
                self.refreshRecovery()
                do { try await sleepLoop(delay) } catch { return }
            }
        }
    }
    /// Also used by the offline integration harness with a fake transport.
    func runAutomaticCycle() async {
        guard !credentialsDeletionInProgress else { return }
        await refreshNetworkEnvironmentInBackground()
        guard !Task.isCancelled, let operation = policy.begin(.automatic), !sleeping else { return }
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
        activeInternetCheck = nil
        invalidateSystemConnectivity()
        if isVerifyingMemory && (memoryVerificationMessage == SharedCopy.t("memory.verification.checking")
            || memoryVerificationMessage == SharedCopy.t("memory.verification.pending")) {
            memoryVerificationMessage = SharedCopy.t("memory.verification.cancelled")
        }
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
    }
    private func startManual(mayAuthenticate: Bool, force: Bool = false) {
        guard !credentialsDeletionInProgress else { return }
        refreshNetworkEnvironment()
        guard policy.foreground, !sleeping else { return }
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
        manual = Task { [weak self] in
            guard let self else { return }
            await self.perform(operation, mayAuthenticate: mayAuthenticate, force: force)
            if self.policy.active == nil { self.startLoop(initialDelay: self.recommendedCheckDelay) }
        }
    }
    private func perform(_ operation: ConnectionPolicy.Operation, mayAuthenticate requestedAuthentication: Bool,
                         force: Bool = false) async {
        let supplementalCheck = operation.kind == .automatic && recoveryFullCheckPending && probeMode == .complete
        let mayAuthenticate = requestedAuthentication && !supplementalCheck
        // Immutable operation inputs; configuration edits invalidate the operation owner.
        let authenticationArea: CampusArea
        if operation.kind == .automatic, area == .automatic, authenticationRoute != nil, networkRemembered,
           let record = memory?.records.first(where: { $0.id == memoryMatch.record }),
           record.area == "teaching", record.proofTeaching && !record.proofDormitory {
            authenticationArea = .teaching
        } else { authenticationArea = area }
        // A quick success proves only one public endpoint. Resolve an uncertain
        // submission with a complete, read-only round without changing the preference.
        let awaitingSubmissionProof = pendingRecoverySubmission || policy.holds.holds.contains { $0.reason == .uncertain }
        let prioritizedRound = operation.kind == .automatic && !awaitingSubmissionProof && canPrioritizeRecovery
        let selectedProbeMode: ConnectivityProbeMode = awaitingSubmissionProof ? .complete : (prioritizedRound ? .quick : probeMode)
        let selectedWebsites = customWebsiteTargets
        let selectedWebsiteStrategy = customWebsiteStrategy
        let checkRevision = networkRevision
        let memoryGen = memoryGeneration
        let credentials = operation.kind == .automatic ? automaticCredentials
            : Credentials(account: account.trimmingCharacters(in: .whitespacesAndNewlines), password: password)
        let hasCredentials = !(credentials?.account.isEmpty ?? true) && !(credentials?.password.isEmpty ?? true)
        var failedRound = false
        defer {
            if probeOwner == operation { probeOwner = nil }
            if policy.owns(operation), !Task.isCancelled, failedRound, operation.kind == .automatic { policy.failed() }
            if policy.finish(operation) {
                partialRecoveryInProgress = false
                busy = false
                if operation.kind == .manual { manual = nil }
                refreshRecovery()
            }
        }
        guard await prepareTransport(for: operation) else { return }
        if !isOnline || operation.kind == .manual { status = SharedCopy.t("status.checking.headline"); engineStatus = "checking" }
        probeOwner = operation
        let boundSubmissionProof = awaitingSubmissionProof && requiresPhysicalBinding && authenticationRoute != nil
        let internet = await runInternetCheck(mode: selectedProbeMode, targets: selectedWebsites, strategy: selectedWebsiteStrategy,
            prioritizeCampusRecovery: operation.kind == .automatic && mayAuthenticate && requiresPhysicalBinding && authenticationRoute != nil,
            deferWebsites: boundSubmissionProof)
        guard !Task.isCancelled, policy.owns(operation), checkRevision == networkRevision else { return }
        if probeOwner == operation { probeOwner = nil }
        let report = internet.base
        acceptReport(report, internet: internet)
        if selectedProbeMode == .complete { recoveryFullCheckPending = false }
        if report.online, rapidRecoveryDeadline != nil {
            rapidRecoveryDeadline = nil
            recoveryFullCheckPending = prioritizedRound && probeMode == .complete
        }
        if operation.kind == .automatic && internet.websitesDeferred && internet.completeBasePassed && probeMode == .complete {
            recoveryFullCheckPending = true
        }
        let partial = operation.kind == .automatic ? internet.hasCampusRecoveryEvidence : internet.hasRecoveryEvidence
        if operation.kind == .automatic {
            partialRecovery.observe(partial: partial, transportRecovered: internet.transportRecovered)
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
        if !manualRound, requiresPhysicalBinding,
           authenticationRoute == nil || (!networkRemembered && !policy.currentNetworkAuthorized) {
            engineStatus = authenticationRoute == nil ? "unconfirmed" : "needsTrust"
            status = SharedCopy.t("memory.remember")
            authenticationSummary = SharedCopy.t("memory.notRemembered")
            return
        }
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
        if !manualRound || authenticationArea == .automatic {
            authenticationSummary = SharedCopy.t("status.discovering.headline")
            engineStatus = "discovering"
            discovery = await client.discoverCampus(area: authenticationArea)
            guard !Task.isCancelled, policy.owns(operation) else { return }
            lastDiscovery = discovery
            if memoryVerdict(level: dormitoryMemoryLevel, discovery: discovery, ambiguous: memoryAmbiguous) == .authorized {
                touchMemory(area: discovery.dormitory == .confirmed ? .dormitory : .teaching)
            }
        }
        var target: CampusArea
        switch CampusPlan.decide(manual: manualRound, choice: authenticationArea, discovery: discovery,
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
        status = SharedCopy.t("status.loggingIn.headline"); detail = SharedCopy.t("status.loggingIn.detail")
        engineStatus = "loggingIn"
        let previousSubmissionCooldown = policy.cooldownState
        let previousSubmissionPending = pendingRecoverySubmission
        policy.authenticationAttemptStarted(now: now())
        pendingRecoverySubmission = true
        guard persistRecoveryJournal() else { refreshRecovery(); return }
        // Consume one recovery round, including an optional unsent-candidate fallback below.
        // An explicit manual retry remains available, but a persistent site fault never loops logins.
        partialRecovery.noteAttempt()
        var attempt = await client.attemptLogin(credentials: credentials, area: target)
        guard !Task.isCancelled, policy.owns(operation) else { return }
        recordAuthenticationDiagnostic(attempt, area: target, generation: generation)
        // Same round, still before any credential left the device: the teaching candidate was
        // unreachable or refused a challenge and the dormitory portal is confirmed → try it once.
        if attempt.outcome == .unavailable || attempt.outcome == .challengeRefused,
           target == .teaching, authenticationArea == .automatic {
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
        if attempt.outcome == .unavailable || attempt.outcome == .challengeRefused {
            // These classified results occur before the formal credential submission.
            // Preserve any older submitted request's deadline rather than extending it.
            policy.restoreCooldownState(previousSubmissionCooldown)
        }
        // Cancellation can arrive after a request reached the server. Keep the journal marker
        // until explicit resume, positive connectivity, or a definitive owned result.
        if attempt.outcome == .unavailable || attempt.outcome == .challengeRefused {
            pendingRecoverySubmission = previousSubmissionPending
        } else if attempt.outcome != .cancelled { pendingRecoverySubmission = false }
        if prioritizedRound, attempt.outcome != .unavailable && attempt.outcome != .challengeRefused {
            rapidRecoveryDeadline = nil
            recoveryFullCheckPending = attempt.outcome == .success && probeMode == .complete
        }
        lastAttemptOutcome = attempt.outcome
        switch attempt.outcome {
        case .success:
            let released = policy.recordSuccess(area: target, generation: generation, epoch: networkEpoch)
            policy.authenticationAccepted(now: now(), clearBlock: false, scope: authenticationScope)
            touchMemory(area: target)
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
                    deferWebsites: requiresPhysicalBinding && authenticationRoute != nil)
                guard !Task.isCancelled, policy.owns(operation), checkRevision == networkRevision else { return }
                let verification = verificationReport.base
                acceptReport(verification, internet: verificationReport)
                if verification.online {
                    partialRecovery.observe(partial: verificationReport.hasCampusRecoveryEvidence,
                                            transportRecovered: verificationReport.transportRecovered)
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
            let hold = policy.recordFailure(automaticChoice: authenticationArea == .automatic, area: target,
                                            generation: generation, epoch: networkEpoch, attempt: attempt)
            record(detail)
            if let hold, hold.reason == .credentials, let heldArea = hold.area {
                record(SharedCopy.f("event.holdCreated", ["area": heldArea.title, "scope": SharedCopy.t(hold.sticky ? "hold.scope.sticky" : "hold.scope.network")]))
            }
            refreshRecovery()
        }
    }
    private func acceptReport(_ report: ConnectivityReport, internet: InternetCheckReport? = nil) {
        lastInternetCheck = internet
        websiteConfigurationChanged = false
        if !report.online { offerForcedLogin = false }
        probeSummary = report.summary
        lastCheckedAt = Date()
        lastDiagnosis = report.diagnosis
        lastProbeMode = report.mode
        lastProbePassedCount = report.passedCount
        let sample = ConnectionSample(report: report, interface: interfaceSummary,
            websiteTotal: internet.flatMap { $0.base.mode == .complete ? $0.includedCount : nil },
            websitePassed: internet.flatMap { $0.base.mode == .complete ? $0.websitePassedCount : nil })
        lastLatencyMilliseconds = sample.latencyMilliseconds
        health.append(sample)
        if !isolated { systemProxyActive = NetworkEnvironment.systemProxyEnabled() }
        if !sessionReadOnly, let healthURL {
            do {
                try JSONEncoder().encode(health).write(to: healthURL, options: .atomic)
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: healthURL.path)
                #if os(iOS)
                try FileManager.default.setAttributes([.protectionKey: FileProtectionType.complete], ofItemAtPath: healthURL.path)
                #endif
            } catch { errorMessage = SharedCopy.t("error.writeFailed") }
        }
    }
    private func setOnline() {
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
    private func persistRecoveryJournal() -> Bool {
        guard !sessionReadOnly else { return true }
        guard !isolated || recoveryJournalURL != nil else { return true }
        guard !recoveryJournalBlocked, let url = recoveryJournalURL else {
            policy.suspendAuthentication(.security)
            errorMessage = SharedCopy.t("error.recoveryJournalUnavailable")
            return false
        }
        let journal = RecoveryJournal(generation: savedGeneration, holds: policy.holds.holds,
            notBefore: policy.authenticationNotBefore, pendingSubmission: pendingRecoverySubmission)
        do {
            let data = try journal.encoded()
            if data != lastRecoveryJournalData { try journal.write(url); lastRecoveryJournalData = data }
            return true
        } catch {
            policy.suspendAuthentication(.security)
            errorMessage = SharedCopy.t("error.recoveryJournalWriteFailed")
            return false
        }
    }
    private func refreshRecovery() {
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
    }

    var currentHealth: ConnectionHealth { health.snapshot() }

    var diagnosticExportData: Data {
        struct Export: Encodable {
            let formatVersion: Int
            let exportedAt: Date
            let appVersion: String
            let build: String
            let mode: String
            let summary: String
            let measurement: String
            let samples: [ConnectionSample]
        }
        let report = Export(formatVersion: 1, exportedAt: Date(),
            appVersion: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.10.1",
            build: Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "32",
            mode: monitoringMode.rawValue, summary: diagnosticSummary,
            measurement: SharedCopy.t("records.measurement"), samples: currentHealth.samples)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return (try? encoder.encode(report)) ?? Data("{}".utf8)
    }

    var diagnosticSummary: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.10.1"
        var environment: [String] = []
        if tunnelActive { environment.append(SharedCopy.t("env.tunnel.detected")) }
        if systemProxyActive { environment.append(SharedCopy.t("diagnostic.systemProxy")) }
        let environmentText: String = environment.isEmpty ? SharedCopy.t("diagnostic.noProxyObserved") : environment.joined(separator: "，")
        let diagnosisText: String = lastDiagnosis?.title ?? SharedCopy.t("connection.neverChecked")
        let checkedText: String = lastCheckedAt?.formatted(date: .numeric, time: .standard) ?? SharedCopy.t("connection.neverChecked")
        var lines: [String] = [
            (SharedCopy.t("app.name") + " ") + version,
            SharedCopy.t("diagnostic.field.state") + (websiteConfigurationChanged ? SharedCopy.t("internet.pending.title") : status),
            SharedCopy.t("diagnostic.field.interface") + ConnectionSample.interfaceLabel(interfaceSummary),
            SharedCopy.t("diagnostic.field.environment") + environmentText,
            SharedCopy.t("diagnostic.field.websiteStrategy") + customWebsiteStrategy.title,
            SharedCopy.t("diagnostic.field.results") + probeSummary,
            SharedCopy.t("diagnostic.field.websites") + (lastInternetCheck.map { $0.base.mode == .complete ? SharedCopy.f("diagnostic.websiteCount", ["passed": String($0.websitePassedCount), "total": String($0.includedCount)]) : SharedCopy.t("internet.quickSkips") } ?? SharedCopy.t("connection.neverChecked")),
            SharedCopy.t("diagnostic.field.clues") + diagnosisText,
            SharedCopy.t("diagnostic.field.mode") + monitoringMode.title,
            SharedCopy.t("diagnostic.field.recovery") + recoverySummary,
            SharedCopy.t("diagnostic.field.authentication") + authenticationSummary,
            SharedCopy.t("diagnostic.field.trust") + authorizationStatusText,
            SharedCopy.t("diagnostic.field.trustDetail") + authorizationReason,
            SharedCopy.t("diagnostic.field.checkedAt") + checkedText
        ]
        if let lastAuthDiagnosticSummary { lines.append(SharedCopy.t("diagnostic.field.authenticationLast") + lastAuthDiagnosticSummary) }
        return lines.joined(separator: "\n")
    }
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
        record(SharedCopy.t("diagnostic.field.authenticationEvent") + fields.joined(separator: " "))
    }
    private func record(_ message: String) {
        let now = Date()
        // Path flapping and repetitive failures should not overwrite useful history.
        if let recent = events.first(where: { $0.message == message }), now.timeIntervalSince(recent.date) < 300 { return }
        events.insert(ConnectionEvent(date: now, message: message), at: 0)
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
