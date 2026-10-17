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
    enum StoreError: LocalizedError {
        case failed(OSStatus)
        var errorDescription: String? { "无法访问系统钥匙串，请解锁设备后重试。" }
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

private let defaultTrustReason = "宿舍区登录会以明文 HTTP 发送账号密码。请确认当前校园网络可信，再允许本次自动重连。"

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
    @Published var status = "等待检测"
    @Published var detail = "连接校园 Wi-Fi 或插上网线后，即可检测或登录。"
    @Published var busy = false { didSet { updateWiFiRecoveryContext() } }
    @Published var events: [ConnectionEvent] = []
    @Published var errorMessage: String?
    @Published private(set) var isOnline = false
    @Published private(set) var interfaceSummary = "正在查看连接方式"
    @Published private(set) var probeSummary = "尚未检测 · Apple / Microsoft"
    @Published private(set) var authenticationSummary = "连续两次检测失败后才会尝试登录"
    private(set) var lastAuthDiagnosticSummary: String?
    @Published private(set) var currentNetworkAuthorized = false
    @Published private(set) var authorizationStatusText = "尚未信任此网络"
    @Published private(set) var authorizationReason = defaultTrustReason
    let isPreview: Bool
    private(set) var legacyUserAtLaunch = false
    @Published private(set) var lastCheckedAt: Date?
    @Published private(set) var nextCheckAt: Date?
    @Published private(set) var recoverySummary = "等待网络连接"
    @Published private(set) var automaticRecoveryPaused = false
    @Published private(set) var sessionReadOnly = false
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
         systemConnectivityProbe: (@Sendable (ConnectivityProbeMode) async -> ConnectivityReport)? = nil) {
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
        if preview { self.systemConnectivityProbe = nil }
        else if let systemConnectivityProbe { self.systemConnectivityProbe = systemConnectivityProbe }
        else if isolated { self.systemConnectivityProbe = nil }
        else { self.systemConnectivityProbe = { await PortalClient.probeSystemConnectivity(mode: $0) } }
        self.automaticLoopAllowed = !preview && (!isolated || runAutomaticLoopInIsolation)
        self.loopSleep = loopSleep ?? { seconds in try await Task.sleep(for: .seconds(seconds)) }
        // Read the user's choices in observe-only mode, but never retain a writable
        // preference store. Injection is limited to isolated, non-preview tests.
        let storedDefaults: UserDefaults? = self.isolated ? (preview ? nil : isolatedPreferences) : .standard
        self.defaults = readOnly ? nil : storedDefaults
        self.sessionReadOnly = readOnly
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
                    } catch { errorMessage = "无法读取系统钥匙串，请解锁设备后重新打开。" }
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
            status = "基础联网检测通过"
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
            errorMessage = "无法读取自动重连的暂停记录，已保留原文件。请检查连接，再到连接页继续自动重连。"
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
                    errorMessage = "连接记录暂不可用，原文件已保留；检测不受影响。"
                }
            }
            logURL = folder.appendingPathComponent("events.json")
            if let logURL, FileManager.default.fileExists(atPath: logURL.path) {
                do {
                    events = try EventLogFile.read(from: logURL)
                } catch {
                    // Keep the unreadable original beside a fresh log instead of overwriting it.
                    if !sessionReadOnly, (try? EventLogFile.quarantine(logURL)) != nil {
                        errorMessage = "无法读取活动记录，已备份原文件。新记录会照常保存。"
                    } else {
                        self.logURL = nil
                        errorMessage = "无法读取活动记录，已保留原文件。本次打开应用期间不再保存活动记录。"
                    }
                }
            }
        } catch { errorMessage = "本地记录暂不可用，请检查存储空间与文件权限。" }
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
            continuity.trustUnavailableReason = "正在确认校园认证使用的物理网络，确认前不会发送账号。"
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
        cancelOperations()
        systemConnectivityRecheckNeeded = recheckSystem && usable && !sleeping && monitoringMode != .manual
        if !firstPath { invalidateTransport() }
        networkRevision &+= 1
        lastInternetCheck = nil
        websiteConfigurationChanged = false
        if !sameIdentity || authenticationTunnelChanged || descriptorsChanged || trustScopeChanged || bindingChanged {
            memoryVerificationMessage = nil
            // A different network: holds scoped to the previous network no longer apply.
            networkEpoch &+= 1
            partialRecovery.reset()
            policy.pathChanged(to: networkEpoch)
            lastDiscovery = nil // evidence about the previous network says nothing about this one
        }
        policy.available = usable && !sleeping
        interfaceSummary = usable && !interfaces.isEmpty ? interfaces : "没有可用的 Wi-Fi 或有线连接"

        if !usable, sameIdentity, linkActive, !sleeping, !descriptorsChanged, !authenticationTunnelChanged, !trustScopeChanged, !bindingChanged,
           identity?.permitsBriefInterruption == true, policy.currentNetworkAuthorized {
            if authorizationGraceDeadline == nil {
                authorizationGraceDeadline = now.addingTimeInterval(15)
                scheduleAuthorizationGraceExpiry()
            }
            if let deadline = authorizationGraceDeadline, now > deadline {
                revokeAuthorization(reason: "网络持续不可用，已取消信任。重新连接后可以再次确认。")
            } else {
                authorizationReason = "网络暂时不通，连接尚未断开，正在等待恢复。"
            }
        } else if !sameIdentity || authenticationTunnelChanged || descriptorsChanged || trustScopeChanged || bindingChanged || !linkActive || sleeping || !usable {
            let reason: String
            if firstPath { reason = defaultTrustReason }
            else if !linkActive { reason = "网络连接已断开，重新连接后请再次确认。" }
            else if identity == nil { reason = "无法确认当前网络，已暂停信任。" }
            else if !usable { reason = "网络暂时不可用，已取消信任。恢复后请再次确认。" }
            else if tunnelChanged { reason = "代理或 VPN 状态已变化，请重新确认当前网络。" }
            else { reason = "网络已变化，请重新确认当前网络。" }
            revokeAuthorization(reason: reason)
            policy.resetFailures()
        } else if let deadline = authorizationGraceDeadline {
            if now > deadline { revokeAuthorization(reason: "网络中断超过 15 秒，请重新确认当前网络。") }
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
        probeSummary = firstPath ? "等待首次检测" : "网络状态更新，等待检测"
        authenticationSummary = "先检测，再判断是否需要登录"
        refreshAuthorization()
        if sleeping { refreshRecovery(); return }
        if usable {
            status = "等待检测"
            detail = policy.automaticEnabled ? "网络已就绪，开始自动检测。" : "网络已就绪，需要时可以检测或登录。"
            startLoop(initialDelay: rapidRecoveryDeadline != nil ? 0 : 2)
        } else {
            status = "尚未连接网络"
            detail = "请连接校园 Wi-Fi 或插上网线。"
            if previouslyAvailable { record("网络连接暂不可用，已停止本次操作。") }
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
            self.revokeAuthorization(reason: "网络持续不可用，已取消信任。重新连接后可以再次确认。")
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
            authorizationStatusText = policy.available ? "已信任此网络" : "仍信任此网络，等待连接恢复"
            if policy.available {
                authorizationReason = "信任本次有效。检测、取消操作或修改账号不会取消信任；换网、休眠或退出后需重新确认。"
            }
        } else {
            authorizationStatusText = "尚未信任此网络"
        }
    }

    private func configurationChanged() {
        guard initialized else { return }
        let interrupted = busy
        cancelOperations()
        policy.resetFailures()
        if interrupted {
            // The stopped round must not leave a stale "正在登录" behind.
            status = isOnline ? "基础联网检测通过" : "已取消"
            detail = "账号或区域已修改，本次操作已停止，网络信任保持不变。"
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
        guard !sessionReadOnly else { return }
        let credentials = Credentials(account: account.trimmingCharacters(in: .whitespacesAndNewlines), password: password)
        guard !credentials.account.isEmpty, !credentials.password.isEmpty,
              credentials.account.utf8.count <= 256, credentials.password.utf8.count <= 1024 else {
            errorMessage = "请填写有效的校园账号和密码。"; return
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
            record("账号密码已存入本机钥匙串，自动重连会使用这个账号。")
            refreshRecovery()
        } catch { errorMessage = "保存失败，原来的账号密码仍保留。请解锁设备后重试。" }
    }
    /// Restores the saved account into the draft. Never touches the Keychain.
    func revertCredentialDraft() {
        account = automaticCredentials?.account ?? ""
        password = automaticCredentials?.password ?? ""
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
    var canAuthenticate: Bool { !sessionReadOnly && monitoringMode != .observe }
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
        refreshNetworkEnvironment()
        refreshSystemConnectivity()
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
            websiteConfigurationError = "网址清单未能读取，原文件已保留。可重新添加并保存，旧文件会另存备份。"
        }
    }
    var selectedWebsiteCount: Int { customWebsiteTargets.filter(\.enabled).count }
    var canEditWebsiteTargets: Bool { !sessionReadOnly && (!busy || probeOwner != nil) }

    @discardableResult
    func saveCustomWebsites(_ targets: [WebsiteTarget], preferComplete: Bool = true, strategy: WebsiteProbeStrategy? = nil) -> Bool {
        guard !sessionReadOnly else { return false }
        let selectedStrategy = strategy ?? customWebsiteStrategy
        guard canEditWebsiteTargets else {
            websiteConfigurationError = "正在检查登录服务或登录校园网，完成后才能修改检测项目。"; return false
        }
        let validated: [WebsiteTarget]
        do { validated = try WebsiteTarget.validated(targets) }
        catch { websiteConfigurationError = (error as? LocalizedError)?.errorDescription ?? "网站设置有误，请检查。"; return false }
        do {
            if !isolated || websitePreferencesURL != nil {
                guard let url = websitePreferencesURL else { throw CocoaError(.fileWriteUnknown) }
                if websitePreferencesCorrupt { try WebsitePreferences.quarantine(url) }
                try WebsitePreferences(targets: validated, strategy: selectedStrategy).write(url)
                websitePreferencesCorrupt = false
            }
        } catch { websiteConfigurationError = "网址清单未能保存，请检查本机存储空间；当前检测项目没有改变。"; return false }
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
        return observedNetworkLabel?.displayName ?? (route.descriptor?.kind == "wifi" ? "当前校园 Wi-Fi" : "当前校园有线网络")
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
        status = "正在检查校园网络"
        authenticationSummary = "正在检查校园网登录服务，没有发送账号密码"
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
                self.authenticationSummary = "检查未通过，没有发送账号密码，也没有记住网络"
                return
            }
            switch self.rememberVerifiedNetwork(scope: scope) {
            case .remembered:
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.remembered")
            case .failed:
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.writeFailed")
            default:
                self.memoryVerificationMessage = SharedCopy.t("memory.verification.changed")
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
        authenticationSummary = dormitory ? "已信任并记住此网络；仍需连续两次检测失败才会登录"
            : "已记住此校园网络，供自动找网使用；教学区自动重连仍需当次校园服务确认"
        record("已记住当前网络，本次没有发送账号密码。")
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
            revokeAuthorization(reason: "已取消信任此网络，检测会继续。")
            authenticationSummary = "已忘记此网络，宿舍区不会自动重连"
        }
        // A failed write still revokes this record in RAM for the rest of this process.
        _ = commitMemory(next, suppress: ids)
        record("已忘记一个记住的网络。")
        objectWillChange.send()
        if isCurrent { startLoop() }
    }

    /// Revokes trust, cancels running work and deletes the records that represent the current network.
    func forgetCurrentNetwork() {
        guard !sessionReadOnly else { return }
        memoryVerificationMessage = nil
        cancelOperations()
        memoryGeneration &+= 1
        revokeAuthorization(reason: "已取消信任此网络，检测会继续。")
        if var next = memory {
            let ids = next.matchingIDs(currentDescriptors, now: now())
            for id in ids { next.forget(id: id) }
            if !ids.isEmpty { _ = commitMemory(next, suppress: ids) }
        }
        authenticationSummary = "已忘记此网络，宿舍区不会自动重连"
        record("已忘记当前网络，需要重新确认信任。")
        objectWillChange.send()
        startLoop()
    }

    func forgetAllNetworks() {
        guard !sessionReadOnly else { return }
        memoryVerificationMessage = nil
        cancelOperations()
        memoryGeneration &+= 1
        revokeAuthorization(reason: "已忘记全部记住的网络。")
        if var next = memory {
            let ids = next.records.map(\.id)
            next.forgetAll()
            if !ids.isEmpty { _ = commitMemory(next, suppress: ids) }
        }
        record("已忘记全部记住的网络。")
        objectWillChange.send()
        startLoop()
    }

    private func loadNetworkMemory(_ url: URL) {
        memoryURL = url
        do {
            if let data = try NetworkMemoryFile.read(url) {
                if let decoded = NetworkMemory.decode(data, now: now()), decoded.hasSalt { memory = decoded }
                else { memoryUnreadable = true; errorMessage = "无法读取记住的网络，已保留原文件。本次需手动信任当前网络。" }
            } else {
                memory = NetworkMemory(salt: NetworkMemory.randomHex())
            }
        } catch {
            memoryUnreadable = true
            errorMessage = "无法读取记住的网络，已保留原文件。本次需手动信任当前网络。"
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
            networkNameError = "网络名称暂不可用，原文件已保留；已有网络记忆不受影响。"
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
            networkNameError = "未能保存网络名称，请检查存储空间和文件权限。"
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
            errorMessage = "未能保存网络，原来记住的网络没有改变。"
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
                authorizationReason = "本次启动仅检测，不能更改信任。"
            } else if policy.available && currentIdentity == nil && lastLinkActive {
                authorizationReason = "暂时无法识别当前网络，不能信任此网络。仍可手动登录。"
            } else {
                authorizationReason = "网络信息还没就绪，等 Wi-Fi 或网线连接稳定后再试。"
            }
            refreshAuthorization()
            return
        }
        policy.authorizeCurrentNetwork()
        clearAuthorizationGrace()
        refreshAuthorization()
        authenticationSummary = "已信任此网络；仍需连续两次检测失败才会登录"
        record("已信任当前网络，本次没有发送账号密码。")
        if rapidRecoveryDeadline != nil { rapidRecoveryDeadline = now().addingTimeInterval(45) }
        if engineStatus == "needsTrust" { engineStatus = "idle" }
        if policy.active == nil {
            stopLoop()
            startLoop(initialDelay: 0)
        }
    }
    func revokeCurrentNetworkAuthorization() {
        cancelOperations()
        revokeAuthorization(reason: "已取消信任此网络，检测会继续。")
        authenticationSummary = "已取消信任，宿舍区不会自动重连"
        startLoop()
    }
    func resumeAutomaticRecovery() {
        guard !sessionReadOnly, monitoringMode == .recover, automaticCredentials != nil else { return }
        cancelOperations()
        if recoveryJournalBlocked, let url = recoveryJournalURL {
            do {
                if FileManager.default.fileExists(atPath: url.path) { _ = try EventLogFile.quarantine(url) }
                recoveryJournalBlocked = false; lastRecoveryJournalData = nil
            } catch { errorMessage = "无法备份原来的暂停记录，暂时不能恢复自动重连。"; return }
        }
        pendingRecoverySubmission = false
        policy.resumeAuthentication()
        authenticationSummary = "已恢复自动重连，网络信任保持不变"
        refreshRecovery()
        startLoop()
    }
    /// Explicit user pause of automatic login (checks continue). Resume clears every hold.
    func pauseAutomaticRecovery() {
        guard monitoringMode == .recover else { return }
        if policy.active?.kind == .automatic { cancelOperations() }
        policy.pauseByUser()
        clearPriorityRecovery()
        record("你暂停了自动重连；检测会继续。")
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
        policy.holds.strongest.map { ConnectionPolicy.AuthenticationBlock($0.reason).message } ?? "自动重连已暂停，检测会继续。"
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
        status = isOnline ? "基础联网检测通过" : "已取消"
        engineStatus = isOnline ? "online" : "cancelled"
        detail = "已停止本次操作，没有断开网络。"
        authenticationSummary = "本次操作已取消，网络信任保持不变"
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
            status = "等待手动检测"; detail = "需要时点“检测连接”或“登录校园网”。"
        }
        authenticationSummary = mode == .recover ? "连续两次检测失败后才会尝试登录" : "当前方式不会自动重连"
        // Mode switches never release a credential or security rejection block.
        if mode != .manual { policy.resetFailures(); startLoop(initialDelay: canPrioritizeRecovery ? 0 : 2) }
        record("已选择：" + mode.title + "。")
        refreshRecovery()
    }
    func setLaunchAtLogin(_ enabled: Bool) {
        #if os(macOS)
        guard !sessionReadOnly, !isolated else { return }
        do {
            if enabled { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
            launchAtLogin = SMAppService.mainApp.status == .enabled
            if enabled && !launchAtLogin { errorMessage = "请在系统设置的登录项中允许深大随连。" }
        } catch {
            launchAtLogin = SMAppService.mainApp.status == .enabled
            errorMessage = "无法更新登录项，请在系统设置中检查。"
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
        policy.resetForPathChange()
        revokeAuthorization(reason: "Mac 已休眠，唤醒后需重新确认当前网络。")
        currentIdentity = nil
        lastLinkActive = false
        policy.available = false // A pre-sleep snapshot cannot authorize the post-wake network.
        isOnline = false
        lastCheckedAt = nil
        lastDiagnosis = nil
        status = value ? "随系统休眠" : "正在恢复"
        detail = value ? "唤醒后会重新检测网络。" : "等待网络稳定后再检测。"
        probeSummary = "等待重新检测"
        authenticationSummary = "休眠后需要重新信任网络"
        if !value, !isolated, let path = monitor?.currentPath {
            receive(PathObservation.capture(path))
            authorizationReason = "已重新识别网络，请再次确认是否信任。"
        }
        refreshAuthorization()
        refreshRecovery()
    }
    private func stopLoop() {
        loop?.cancel(); loop = nil; loopID = nil; nextCheckAt = nil
    }
    private func startLoop(initialDelay: Int = 2) {
        guard automaticLoopAllowed, policy.canRunAutomatic, !sleeping, loop == nil else { return }
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
            status = "尚未连接网络"; detail = "请连接校园 Wi-Fi 或插上网线。"; return
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
        if !isOnline || operation.kind == .manual { status = "正在" + selectedProbeMode.title; engineStatus = "checking" }
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
                    detail = "基础检测已通过，本次没有发送账号密码。如需重新登录，点“仍要登录”。"
                    if internet.hasOnlyWebsiteIssues {
                        detail = SharedCopy.t("internet.websiteIssues.detail") + "\n\n" + detail
                    }
                }
                return
            }
        } else {
            if operation.kind == .automatic { policy.observeProbe(online: false) }
            if isOnline { record("外网检测暂未通过，稍后再检测一次。") }
            isOnline = false
            engineStatus = "offline"
            failedRound = true
        }
        guard mayAuthenticate && canAuthenticate else {
            if report.online { return }
            status = report.resultTitle
            detail = report.diagnosis.explanation
            authenticationSummary = "仅检测，没有发送账号密码"
            if operation.kind == .manual { record("手动检测未通过：" + report.diagnosis.title + "。没有发起登录。") }
            return
        }
        guard let credentials, hasCredentials else {
            engineStatus = "needsAccount"
            status = "请填写校园网账号"
            detail = operation.kind == .automatic ? "保存账号密码后才能自动重连，检测会继续。" : "请先填写账号和密码。"
            authenticationSummary = "缺少账号密码，没有发送登录请求"
            return
        }
        let generation = credentialGeneration(for: credentials)
        let manualRound = operation.kind == .manual
        if !manualRound, requiresPhysicalBinding,
           authenticationRoute == nil || (!networkRemembered && !policy.currentNetworkAuthorized) {
            engineStatus = authenticationRoute == nil ? "unconfirmed" : "needsTrust"
            status = "请核验并记住这个新网络"
            authenticationSummary = "当前网络未在已记住的名单中，没有发送账号密码"
            return
        }
        if !manualRound {
            if pausedForAutomatic {
                status = "自动重连已暂停"; detail = strongestHoldMessage
                authenticationSummary = "继续检测，等待你恢复自动重连"
                engineStatus = "paused"
                return
            }
            if cooldownRemaining != nil {
                status = "等待登录生效"; detail = "刚刚发送过登录请求，90 秒内不会自动重试，期间会继续检测。"
                authenticationSummary = "等待登录结果，期间只检测、不重复登录"
                engineStatus = "coolingDown"
                return
            }
            guard partialAutomatic || policy.consecutiveProbeFailures >= 2 else {
                let seconds = canPrioritizeRecovery ? 1 : 5
                status = "稍后再次检测"; detail = report.diagnosis.explanation + "\(seconds) 秒后再确认一次。"
                authenticationSummary = "检测失败 1 次，再失败 1 次才会尝试登录"
                engineStatus = "confirming"
                return
            }
        }
        partialRecoveryInProgress = partialAutomatic
        // AUTO-RECOVERY-01: both entry paths discover the campus first; a pinned manual area skips it.
        var discovery = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
        if !manualRound || authenticationArea == .automatic {
            authenticationSummary = "正在检查校园网登录服务"
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
            status = manualRound ? "尚未确认校园网登录服务" : "暂不自动重连"
            #if os(macOS)
            if tunnelActive {
                detail = "还无法确认校园服务或物理直连通道，没有发送账号密码。请检查当前校园网连接后重试。"
            } else if systemProxyActive {
                detail = "还无法确认校园网登录服务。请确认校园网连接与系统代理设置后重试。"
            } else {
                detail = "还无法确认校园网登录服务，稍后继续检测。"
            }
            #else
            if tunnelActive || systemProxyActive {
                detail = "还无法确认校园网登录服务。检测到 VPN 或代理，请将校园网登录地址设为不使用代理。"
            } else {
                detail = "还无法确认校园网登录服务，稍后继续检测。"
            }
            #endif
            authenticationSummary = "没有发送账号密码"
            engineStatus = "unconfirmed"
            return
        case .needsTrust:
            status = "暂不自动重连"
            detail = "宿舍区需要你先信任当前网络，才能自动重连。"
            authenticationSummary = "没有发送账号密码"
            engineStatus = "needsTrust"
            return
        case .paused:
            status = "自动重连已暂停"; detail = strongestHoldMessage
            authenticationSummary = "继续检测，等待你恢复自动重连"
            engineStatus = "paused"
            return
        }
        guard !Task.isCancelled, policy.owns(operation) else { return }
        // A forget / revoke while this round was in flight must not be overtaken by its stale plan.
        guard memoryGen == memoryGeneration else { return }
        status = "正在登录"; detail = "正在连接校园网登录服务。"
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
            record(attempt.message ?? "登录已通过")
            if !released.isEmpty { record(target.title + "已恢复自动重连。") }
            refreshRecovery()
            // At most three read-only confirmations. Never submit credentials again here.
            engineStatus = "verifying"
            for delay in [0, 2, 5] {
                if delay > 0 && !isolated { try? await Task.sleep(for: .seconds(delay)) }
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
            isOnline = false; status = "登录已提交，等待联网"
            detail = "校园网已接受登录，联网检测暂未通过。90 秒内会继续检测，不会自动再次登录。"
            authenticationSummary = "等待登录生效或外网恢复"
            engineStatus = "accepted"
            record("登录已通过，外网检测尚未通过，暂时只检测、不重复登录。")
        case .cancelled:
            preservePendingSubmission()
            failedRound = false
        default:
            isOnline = report.online
            status = "登录未完成"
            // Never expose an arbitrary transport error carrying a credential-bearing URL.
            detail = attempt.message ?? "校园网登录服务暂时连不上，请稍后再试。"
            engineStatus = attempt.outcome.engineStatus
            let hold = policy.recordFailure(automaticChoice: authenticationArea == .automatic, area: target,
                                            generation: generation, epoch: networkEpoch, attempt: attempt)
            record(detail)
            if let hold, hold.reason == .credentials, let heldArea = hold.area {
                record(heldArea.title + "拒绝了登录：" + (hold.sticky ? "已长期" : "在当前网络") + "暂停该区域的自动重连。")
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
            } catch { errorMessage = "连接记录未能保存，请检查可用存储空间。" }
        }
    }
    private func setOnline() {
        let completelyPassed = requiresPhysicalBinding && authenticationRoute != nil
            ? lastInternetCheck?.completeBasePassed == true : lastInternetCheck?.verdict == .allPassed
        if completelyPassed {
            pendingRecoverySubmission = false
            policy.verifyAcceptedScope(authenticationScope, now: now())
        }
        if !isOnline { record("基础联网检测通过。") }
        // One reachable endpoint, including a quick check, cannot settle an unknown submission.
        if completelyPassed, policy.settleUncertainAuthentication() {
            record("完整基础联网检测已通过，上次登录结果已不影响使用。")
        }
        authenticationSummary = "基础检测已通过"
        engineStatus = "online"
        isOnline = true
        policy.resetFailures()
        if let internet = lastInternetCheck, internet.base.mode == .complete {
            if internet.websitesDeferred {
                status = "基础联网检测通过"
                detail = "校园联网已恢复，常用网址稍后单独检测。"
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
            status = "快速检测通过"
            detail = "至少一个检测地址可以访问。另一项若仍在等待，会结束检测，不算失败。"
        }
    }
    @discardableResult
    private func persistRecoveryJournal() -> Bool {
        guard !sessionReadOnly else { return true }
        guard !isolated || recoveryJournalURL != nil else { return true }
        guard !recoveryJournalBlocked, let url = recoveryJournalURL else {
            policy.suspendAuthentication(.security)
            errorMessage = "自动重连记录暂时不可用，已暂停登录，检测会继续。"
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
            errorMessage = "自动重连记录保存失败，已停止继续登录。请检查存储空间和权限。"
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
        else if sleeping { recoverySummary = "休眠中，唤醒后重新检测" }
        else if monitoringMode == .observe { recoverySummary = "定时检测，不会自动重连" + next }
        else if !autoReconnect { recoverySummary = "需要时手动检测" }
        else if pausedForAutomatic { recoverySummary = strongestHoldMessage }
        else if !policy.available { recoverySummary = "等待 Wi-Fi 或有线网络" }
        else if automaticCredentials == nil { recoverySummary = "正在检测，保存账号密码后才能自动重连" }
        else if isOnline { recoverySummary = "已联网，掉线时自动重连" + next }
        else if cooldownRemaining != nil { recoverySummary = "避免重复登录，15 秒后复查当前连接" }
        else if nextCheckAt != nil { recoverySummary = "掉线时自动重连" + next }
        else { recoverySummary = "正在检测" }
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
            measurement: "HTTP 探测响应时间，包含连接与服务响应；不是网速、ICMP 延迟或连续在线率。", samples: currentHealth.samples)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return (try? encoder.encode(report)) ?? Data("{}".utf8)
    }

    var diagnosticSummary: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.10.1"
        var environment: [String] = []
        if tunnelActive { environment.append("检测到 VPN / 代理隧道") }
        if systemProxyActive { environment.append("系统代理已开启（基础检测与校园认证忽略系统代理；自选网址使用所选策略）") }
        let environmentText: String = environment.isEmpty ? "未检测到系统代理或隧道" : environment.joined(separator: "，")
        let diagnosisText: String = lastDiagnosis?.title ?? "尚未检测"
        let checkedText: String = lastCheckedAt?.formatted(date: .numeric, time: .standard) ?? "尚未检测"
        var lines: [String] = [
            "深大随连 " + version,
            "状态：" + (websiteConfigurationChanged ? "等待按当前配置检测" : status),
            "接口：" + ConnectionSample.interfaceLabel(interfaceSummary),
            "网络环境：" + environmentText,
            "网址检测方式：" + customWebsiteStrategy.title,
            "检测结果：" + probeSummary,
            "自选网站：" + (lastInternetCheck.map { $0.base.mode == .complete ? "\($0.websitePassedCount)/\($0.includedCount) 项通过（不含网址）" : "快速模式未检查" } ?? "尚未检测"),
            "可能原因：" + diagnosisText,
            "运行方式：" + monitoringMode.title,
            "恢复：" + recoverySummary,
            "登录限制：" + authenticationSummary,
            "网络信任：" + authorizationStatusText,
            "信任说明：" + authorizationReason,
            "最近检测：" + checkedText
        ]
        if let lastAuthDiagnosticSummary { lines.append("最近登录诊断：" + lastAuthDiagnosticSummary) }
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
        record("登录诊断：" + fields.joined(separator: " "))
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
        } catch { errorMessage = "记录写入失败，请检查可用存储空间。" }
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
