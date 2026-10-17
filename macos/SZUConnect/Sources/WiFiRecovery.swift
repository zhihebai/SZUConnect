#if os(macOS)
import Foundation
import Combine
import AppKit
import CoreWLAN
import CoreLocation
import SystemConfiguration
import CryptoKit
import Darwin

/// Local display metadata only. A remembered SSID never grants campus authentication trust.
struct WiFiRecoveryNetwork: Identifiable, Equatable, Sendable {
    let id: String
    let ssid: String
    let displayName: String

    var ssidDigest: String { SHA256.hash(data: Data(ssid.utf8)).map { String(format: "%02x", $0) }.joined() }
    var isValid: Bool {
        !id.isEmpty && id.utf8.count <= 128 && (1...32).contains(ssid.utf8.count)
            && !ssid.unicodeScalars.contains(where: CharacterSet.controlCharacters.contains)
    }
}

enum WiFiNamePermission: Equatable { case notRequested, authorized, denied }
enum WiFiRecoverySetupIssue: Equatable {
    case unreadable, permissionRequired, noKnownNetworks, noSelection
}
enum WiFiRecoveryStatus: Equatable {
    case disabled, paused, connected, noSelection, searching, waiting, wifiOff
    case poweredOn, joined, systemConnectionRequired, permissionRequired, unavailable, connectionUnclear, failed
}

/// Cancellation remains visible while a synchronous CoreWLAN scan is blocking its background queue.
final class WiFiRecoveryAuthorization: @unchecked Sendable {
    private let lock = NSLock()
    private var valid = true
    var isValid: Bool { lock.lock(); defer { lock.unlock() }; return valid }
    func invalidate() { lock.lock(); valid = false; lock.unlock() }
}

struct WiFiRecoveryRequest: Sendable {
    let networks: [WiFiRecoveryNetwork]
    let allowPowerOn: Bool
}

enum WiFiRecoveryResult: Equatable, Sendable {
    case cancelled, alreadyConnected, notFound, wifiOff, poweredOn, joined
    case systemConnectionRequired, permissionRequired, noSelection, unavailable, connectionUnclear, failed
}

protocol WiFiRecoveryServicing: Sendable {
    func recover(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) async -> WiFiRecoveryResult
}

/// Deliberately has no CoreWLAN, location, keychain, or system-network calls.
struct PreviewWiFiRecoveryService: WiFiRecoveryServicing {
    func recover(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) async -> WiFiRecoveryResult {
        authorization.isValid ? .notFound : .cancelled
    }
}

struct WiFiRecoveryPreferences: Codable, Equatable {
    struct Selection: Codable, Equatable {
        let id: String
        let ssidDigest: String
    }
    var schemaVersion = 1
    var autoJoinEnabled = false
    var powerOnWhenDisconnected = false
    // Kept only to migrate the former explicit allowlist without broadening an existing choice.
    var selections: [Selection] = []
    // nil means a legacy file awaiting its first complete nonempty remembered-network context.
    var excludedNetworkIDs: [String]? = []

    init() {}

    private enum CodingKeys: String, CodingKey {
        case schemaVersion, autoJoinEnabled, powerOnWhenDisconnected, selections, excludedNetworkIDs
    }
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        schemaVersion = try values.decode(Int.self, forKey: .schemaVersion)
        autoJoinEnabled = try values.decode(Bool.self, forKey: .autoJoinEnabled)
        powerOnWhenDisconnected = try values.decode(Bool.self, forKey: .powerOnWhenDisconnected)
        selections = try values.decode([Selection].self, forKey: .selections)
        excludedNetworkIDs = try values.decodeIfPresent([String].self, forKey: .excludedNetworkIDs)
    }

    func validate() throws {
        let excluded = excludedNetworkIDs ?? []
        guard schemaVersion == 1, selections.count <= 8, Set(selections.map(\.id)).count == selections.count,
              selections.allSatisfy({ !$0.id.isEmpty && $0.id.utf8.count <= 128 && $0.ssidDigest.count == 64
                  && $0.ssidDigest.allSatisfy({ $0.isHexDigit && !$0.isUppercase }) }),
              excluded.count <= 64, Set(excluded).count == excluded.count,
              excluded.allSatisfy({ !$0.isEmpty && $0.utf8.count <= 128
                  && $0 == $0.trimmingCharacters(in: .whitespacesAndNewlines)
                  && !$0.unicodeScalars.contains(where: CharacterSet.controlCharacters.contains) })
        else { throw CocoaError(.coderReadCorrupt) }
    }
    static func read(_ url: URL) throws -> Self {
        guard FileManager.default.fileExists(atPath: url.path) else { return Self() }
        let handle = try FileHandle(forReadingFrom: url); defer { try? handle.close() }
        let data = try handle.read(upToCount: 16_385) ?? Data()
        guard data.count <= 16_384 else { throw CocoaError(.fileReadTooLarge) }
        let value = try JSONDecoder().decode(Self.self, from: data)
        try value.validate()
        return value
    }
    func write(_ url: URL) throws {
        try validate()
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(self)
        let directory = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        let temporary = directory.appendingPathComponent(".wifi-recovery-\(UUID().uuidString).tmp")
        defer { try? FileManager.default.removeItem(at: temporary) }
        try data.write(to: temporary, options: [.atomic, .completeFileProtection])
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: temporary.path)
        guard rename(temporary.path, url.path) == 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
    }
}

@MainActor
final class WiFiRecoveryController: NSObject, ObservableObject, CLLocationManagerDelegate {
    @Published private(set) var autoJoinEnabled = false
    @Published private(set) var powerOnWhenDisconnected = false
    @Published private(set) var selectedNetworkIDs: Set<String> = []
    @Published private(set) var allowedNetworks: [WiFiRecoveryNetwork] = []
    @Published private(set) var status: WiFiRecoveryStatus = .disabled
    @Published private(set) var preferencesUnreadable = false
    @Published private(set) var errorMessage: String?
    @Published private(set) var permissionStatus: WiFiNamePermission = .notRequested
    var onWiFiNamePermissionChanged: (() -> Void)?

    private let preferencesURL: URL?
    private let service: any WiFiRecoveryServicing
    private let startAutomatically: Bool
    private let requestsSystemPermission: Bool
    private let preferencesReadOnly: Bool
    private let retryDelays: [TimeInterval]
    private var preferences = WiFiRecoveryPreferences()
    private var allowRecovery = false
    private var hasPhysicalConnection = true
    private var sleeping = false
    private var authorization: WiFiRecoveryAuthorization?
    private var operation: Task<Void, Never>?
    private var operationID: UUID?
    private var failures = 0
    private var nextAttempt = Date.distantPast
    private var locationManager: CLLocationManager?
    private var permissionActivationObserver: NSObjectProtocol?

    init(preferencesURL: URL?, service: any WiFiRecoveryServicing = CoreWLANRecoveryService(),
         startAutomatically: Bool = true, requestsSystemPermission: Bool = true, preferencesReadOnly: Bool = false,
         retryDelays: [TimeInterval] = [15, 30, 60, 120, 300]) {
        self.preferencesURL = preferencesURL
        self.service = service
        self.startAutomatically = startAutomatically
        self.requestsSystemPermission = requestsSystemPermission
        self.preferencesReadOnly = preferencesReadOnly
        self.retryDelays = retryDelays.isEmpty ? [300] : retryDelays.map { max(0.01, $0) }
        super.init()
        if requestsSystemPermission {
            // Retain a main-thread delegate so System Settings changes remain observable.
            // Creating the manager only reads authorization; it never requests it or starts location updates.
            let manager = CLLocationManager()
            permissionStatus = Self.permission(from: manager.authorizationStatus)
            locationManager = manager
            manager.delegate = self
        }
        if let preferencesURL {
            do { preferences = try WiFiRecoveryPreferences.read(preferencesURL) }
            catch { preferencesUnreadable = true }
        }
        publishPreferences()
        updateIdleStatus()
    }

    deinit {
        authorization?.invalidate(); operation?.cancel()
        if let permissionActivationObserver { NotificationCenter.default.removeObserver(permissionActivationObserver) }
    }

    func updateContext(allowRecovery: Bool, hasPhysicalConnection: Bool, sleeping: Bool,
                       allowedNetworks: [WiFiRecoveryNetwork]) {
        var seen: Set<String> = []
        let networks = Array(allowedNetworks.filter { $0.isValid && seen.insert($0.id).inserted }.prefix(8))
        let contextChanged = self.allowRecovery != allowRecovery || self.hasPhysicalConnection != hasPhysicalConnection
            || self.sleeping != sleeping
        let targetsChanged = self.allowedNetworks.map { WiFiRecoveryPreferences.Selection(id: $0.id, ssidDigest: $0.ssidDigest) }
            != networks.map { WiFiRecoveryPreferences.Selection(id: $0.id, ssidDigest: $0.ssidDigest) }
        guard contextChanged || self.allowedNetworks != networks else { return }
        guard contextChanged || targetsChanged else {
            // Display names are independent of connection authority and retry scheduling.
            self.allowedNetworks = networks
            return
        }
        let wasRecoverable = mayRecover
        let previousSelections = selectedNetworkIDs
        // Revoke first, before any persistence work or UI publication.
        invalidate()
        self.allowRecovery = allowRecovery
        self.hasPhysicalConnection = hasPhysicalConnection
        self.sleeping = sleeping
        self.allowedNetworks = networks
        migrateLegacySelectionsIfNeeded()
        publishPreferences()
        if mayRecover && (!wasRecoverable || !selectedNetworkIDs.subtracting(previousSelections).isEmpty) { resetBackoff() }
        updateIdleStatus()
        scheduleIfNeeded()
    }

    func setAutoJoinEnabled(_ enabled: Bool) {
        guard !preferencesReadOnly, autoJoinEnabled != enabled else { return }
        invalidate()
        var updated = preferences; updated.autoJoinEnabled = enabled
        save(updated, revoking: !enabled)
    }

    func setPowerOnWhenDisconnected(_ enabled: Bool) {
        guard !preferencesReadOnly, powerOnWhenDisconnected != enabled else { return }
        invalidate()
        var updated = preferences; updated.powerOnWhenDisconnected = enabled
        save(updated, revoking: !enabled)
    }

    func setNetworkSelected(_ id: String, selected: Bool) {
        guard !preferencesReadOnly, allowedNetworks.contains(where: { $0.id == id }) else { return }
        guard selectedNetworkIDs.contains(id) != selected else { return }
        invalidate()
        var updated = preferences
        var excluded = Set(updated.excludedNetworkIDs ?? [])
        if selected { excluded.remove(id) }
        else {
            // Never silently discard an older opt-out to make room for a new one.
            guard excluded.count < 64 else {
                updated.autoJoinEnabled = false
                save(updated, revoking: true)
                errorMessage = SharedCopy.t("wifiRecovery.saveFailed")
                return
            }
            excluded.insert(id)
        }
        updated.excludedNetworkIDs = excluded.sorted()
        updated.selections = []
        save(updated, revoking: !selected)
    }

    /// Persist opt-outs before network memory can replace any of these record IDs.
    func preserveExclusionsBeforeMerging(_ ids: [String]) -> Bool {
        let group = Set(ids)
        guard group.count > 1 else { return true }
        guard !preferencesReadOnly, !preferencesUnreadable, existingFileIsReadable() else {
            invalidate(); updateIdleStatus()
            return false
        }
        migrateLegacySelectionsIfNeeded()
        guard let previous = preferences.excludedNetworkIDs else {
            if preferences.selections.isEmpty { return true }
            // Without names, a legacy allowlist cannot yet be matched to exact SSIDs.
            invalidate()
            preferences.autoJoinEnabled = false
            publishPreferences(); updateIdleStatus()
            errorMessage = SharedCopy.t("wifiRecovery.saveFailed")
            return false
        }
        let excluded = Set(previous)
        guard !excluded.isDisjoint(with: group) else { return true }
        invalidate()
        var updated = preferences
        let combined = excluded.union(group)
        guard combined.count <= 64 else {
            updated.autoJoinEnabled = false
            save(updated, revoking: true)
            errorMessage = SharedCopy.t("wifiRecovery.saveFailed")
            return false
        }
        updated.excludedNetworkIDs = combined.sorted()
        updated.selections = []
        return save(updated, revoking: true)
    }

    private func migrateLegacySelectionsIfNeeded() {
        guard preferences.excludedNetworkIDs == nil, !allowedNetworks.isEmpty else { return }
        let legacy = preferences.selections
        preferences.excludedNetworkIDs = legacy.isEmpty ? [] : allowedNetworks.compactMap { network in
            legacy.contains(where: { $0.id == network.id && $0.ssidDigest == network.ssidDigest }) ? nil : network.id
        }.sorted()
        preferences.selections = []
        // Keep this conservative choice for the run if writing fails; never overwrite damaged data.
        persistCurrentPreferences()
    }

    @discardableResult
    private func save(_ updated: WiFiRecoveryPreferences, revoking: Bool) -> Bool {
        let wasRecoverable = mayRecover
        let previousSelections = selectedNetworkIDs
        let previouslyAllowedPowerOn = powerOnWhenDisconnected
        guard !preferencesUnreadable, existingFileIsReadable() else {
            errorMessage = SharedCopy.t("wifiRecovery.unreadable")
            updateIdleStatus(); return false
        }
        var saved = true
        do {
            if let preferencesURL { try updated.write(preferencesURL) }
            preferences = updated
            errorMessage = nil
        } catch {
            if revoking { preferences = updated }
            errorMessage = SharedCopy.t("wifiRecovery.saveFailed")
            saved = false
        }
        publishPreferences()
        if mayRecover && (!wasRecoverable || !selectedNetworkIDs.subtracting(previousSelections).isEmpty
            || (!previouslyAllowedPowerOn && powerOnWhenDisconnected)) { resetBackoff() }
        updateIdleStatus(); scheduleIfNeeded()
        return saved
    }

    private func persistCurrentPreferences() {
        guard !preferencesReadOnly, !preferencesUnreadable, existingFileIsReadable() else { return }
        do {
            if let preferencesURL { try preferences.write(preferencesURL) }
            errorMessage = nil
        } catch { errorMessage = SharedCopy.t("wifiRecovery.revokeSaveFailed") }
    }

    private func existingFileIsReadable() -> Bool {
        guard let preferencesURL else { return true }
        do { _ = try WiFiRecoveryPreferences.read(preferencesURL); return true }
        catch {
            // A file damaged or replaced after launch must not be silently overwritten.
            preferencesUnreadable = true
            errorMessage = SharedCopy.t("wifiRecovery.unreadable")
            publishPreferences()
            return false
        }
    }

    private func publishPreferences() {
        autoJoinEnabled = !preferencesUnreadable && preferences.autoJoinEnabled
        powerOnWhenDisconnected = !preferencesUnreadable && preferences.powerOnWhenDisconnected
        if let excluded = preferences.excludedNetworkIDs {
            selectedNetworkIDs = Set(allowedNetworks.map(\.id)).subtracting(excluded)
        } else {
            selectedNetworkIDs = Set(preferences.selections.compactMap { selection in
                allowedNetworks.contains(where: { $0.id == selection.id && $0.ssidDigest == selection.ssidDigest }) ? selection.id : nil
            })
        }
    }

    var setupIssue: WiFiRecoverySetupIssue? {
        if preferencesUnreadable { return .unreadable }
        if permissionStatus != .authorized { return .permissionRequired }
        if allowedNetworks.isEmpty { return .noKnownNetworks }
        if selectedNetworkIDs.isEmpty { return .noSelection }
        return nil
    }
    private var mayRecover: Bool {
        !preferencesReadOnly && !preferencesUnreadable && autoJoinEnabled && allowRecovery && !hasPhysicalConnection && !sleeping
            && (powerOnWhenDisconnected || setupIssue == nil)
    }
    private func resetBackoff() {
        nextAttempt = .distantPast
        failures = 0
    }
    private func invalidate() {
        authorization?.invalidate()
        operation?.cancel()
    }
    private func updateIdleStatus() {
        if !autoJoinEnabled { status = .disabled }
        else if let setupIssue {
            switch setupIssue {
            case .unreadable: status = .unavailable
            case .permissionRequired: status = .permissionRequired
            case .noKnownNetworks, .noSelection: status = .noSelection
            }
        }
        else if preferencesReadOnly || sleeping || !allowRecovery { status = .paused }
        else if hasPhysicalConnection { status = .connected }
        else { status = .waiting }
    }
    private func scheduleIfNeeded() {
        guard startAutomatically, mayRecover, operation == nil else { return }
        let id = UUID(); operationID = id
        let wait = max(0, nextAttempt.timeIntervalSinceNow)
        operation = Task { [weak self] in
            do { if wait > 0 { try await Task.sleep(for: .seconds(wait)) }; try Task.checkCancellation() }
            catch { self?.finishScheduled(id); return }
            await self?.performAttempt()
            self?.finishScheduled(id)
        }
    }
    private func finishScheduled(_ id: UUID) {
        guard operationID == id else { return }
        operation = nil; operationID = nil
        scheduleIfNeeded()
    }

    /// Offline tests call this with an injected service and startAutomatically: false.
    func refreshOnce() async {
        guard operation == nil else { return }
        let id = UUID(); operationID = id
        // A task handle ensures concurrent refresh calls and context updates cannot overlap scans.
        operation = Task { [weak self] in await self?.performAttempt() }
        await operation?.value
        if operationID == id { operation = nil; operationID = nil }
    }

    private func performAttempt() async {
        guard mayRecover, nextAttempt <= Date() else { return }
        let token = WiFiRecoveryAuthorization(); authorization = token
        nextAttempt = Date().addingTimeInterval(retryDelays[min(failures, retryDelays.count - 1)])
        let selected = allowedNetworks.filter { selectedNetworkIDs.contains($0.id) }
        status = .searching
        let result = await service.recover(.init(networks: selected, allowPowerOn: powerOnWhenDisconnected), authorization: token)
        guard authorization === token, token.isValid, mayRecover, !Task.isCancelled else { return }
        switch result {
        case .cancelled: updateIdleStatus()
        case .alreadyConnected: status = .connected
        case .notFound: status = .waiting
        case .wifiOff: status = .wifiOff
        case .poweredOn: status = .poweredOn
        case .joined: status = .joined
        case .systemConnectionRequired: status = .systemConnectionRequired
        case .permissionRequired: status = .permissionRequired
        case .noSelection: status = .noSelection
        case .unavailable: status = .unavailable
        case .connectionUnclear: status = .connectionUnclear
        case .failed: status = .failed
        }
        if result == .poweredOn || (powerOnWhenDisconnected && (result == .permissionRequired || result == .noSelection)) {
            // Power permission is independent of name access and network selection. Keep checking
            // the radio promptly while setup is incomplete; these checks cannot scan or associate.
            failures = 0
            nextAttempt = Date().addingTimeInterval(min(2, retryDelays[0]))
            return
        }
        let delay = retryDelays[min(failures, retryDelays.count - 1)]
        failures = min(failures + 1, retryDelays.count - 1)
        nextAttempt = Date().addingTimeInterval(delay)
    }

    func requestWiFiNamePermission() {
        guard !preferencesReadOnly else { return }
        guard requestsSystemPermission else {
            // Isolated preview grants only a simulated UI state and never constructs CLLocationManager.
            applyPermissionStatus(.authorized)
            return
        }
        // Permission is user-initiated. Never request coordinates or start location updates.
        // Core Location ignores a WhenInUse request from an inactive app. A menu-bar or
        // accessibility action can reach this button without making the app active first.
        if NSApplication.shared.isActive {
            locationManager?.requestWhenInUseAuthorization()
        } else {
            if permissionActivationObserver == nil {
                permissionActivationObserver = NotificationCenter.default.addObserver(
                    forName: NSApplication.didBecomeActiveNotification, object: NSApplication.shared, queue: .main) { [weak self] _ in
                        Task { @MainActor [weak self] in self?.requestPermissionAfterActivation() }
                    }
            }
            NSApplication.shared.activate()
        }
    }

    private func requestPermissionAfterActivation() {
        if let permissionActivationObserver { NotificationCenter.default.removeObserver(permissionActivationObserver) }
        permissionActivationObserver = nil
        guard !preferencesReadOnly, requestsSystemPermission, NSApplication.shared.isActive else { return }
        locationManager?.requestWhenInUseAuthorization()
    }

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        let newStatus = Self.permission(from: manager.authorizationStatus)
        Task { @MainActor [weak self] in
            self?.applyPermissionStatus(newStatus)
        }
    }
    // Shared by the OS delegate and isolated tests; unchanged initial callbacks must not cancel work.
    func applyPermissionStatus(_ newStatus: WiFiNamePermission) {
        guard permissionStatus != newStatus else { return }
        invalidate()
        permissionStatus = newStatus
        if newStatus == .authorized { resetBackoff() }
        onWiFiNamePermissionChanged?()
        updateIdleStatus(); scheduleIfNeeded()
    }
    nonisolated private static func permission(from value: CLAuthorizationStatus) -> WiFiNamePermission {
        switch value {
        case .authorizedAlways: return .authorized
        case .notDetermined: return .notRequested
        default: return .denied
        }
    }
}

enum WiFiPhysicalState: Equatable { case connected, disconnected, unknown }
struct WiFiScanCandidate {
    let id: UUID
    let ssid: String
    let signal: Int
    let isOpen: Bool
}

/// Used only on CoreWLANRecoveryService's private serial queue, including fake radios in tests.
protocol WiFiRecoveryRadio: AnyObject {
    func permissionGranted() -> Bool
    func physicalState() -> WiFiPhysicalState
    func powerState() -> Bool?
    func setPowerOn() throws
    func scan(ssid: String) throws -> [WiFiScanCandidate]
    func associateOpen(_ candidate: WiFiScanCandidate) throws
}

final class CoreWLANRecoveryService: WiFiRecoveryServicing, @unchecked Sendable {
    private let queue = DispatchQueue(label: "local.SZUConnect.wifi-recovery", qos: .utility)
    private let radio: any WiFiRecoveryRadio
    init(radio: any WiFiRecoveryRadio = CoreWLANRecoveryRadio()) { self.radio = radio }

    func recover(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) async -> WiFiRecoveryResult {
        await withCheckedContinuation { continuation in
            queue.async { [self] in continuation.resume(returning: recoverSerial(request, authorization: authorization)) }
        }
    }
    private func recoverSerial(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) -> WiFiRecoveryResult {
        guard authorization.isValid, request.networks.count <= 8,
              request.networks.allSatisfy(\.isValid) else { return .cancelled }
        func gate() -> WiFiRecoveryResult? {
            guard authorization.isValid else { return .cancelled }
            switch radio.physicalState() {
            case .connected: return .alreadyConnected
            case .unknown: return .connectionUnclear
            case .disconnected: return authorization.isValid ? nil : .cancelled
            }
        }
        if let result = gate() { return result }
        guard let powered = radio.powerState() else { return .unavailable }
        if !powered {
            guard request.allowPowerOn else { return .wifiOff }
            if let result = gate() { return result }
            do {
                // CoreWLAN has no cancellable association/power API. Recheck immediately before entry;
                // invalidation while an OS call is executing rejects its result and all later actions.
                guard authorization.isValid else { return .cancelled }
                try radio.setPowerOn()
                return authorization.isValid ? .poweredOn : .cancelled
            } catch { return authorization.isValid ? .failed : .cancelled }
        }
        guard radio.permissionGranted() else { return .permissionRequired }
        guard !request.networks.isEmpty else { return .noSelection }
        var sawEncrypted = false
        var sawFailure = false
        for ssid in Array(Set(request.networks.map(\.ssid))).sorted() {
            if let result = gate() { return result }
            do {
                // Directed scans only. CoreWLAN can return other networks, so filter again exactly.
                let found = try radio.scan(ssid: ssid).filter { $0.ssid == ssid }
                guard authorization.isValid else { return .cancelled }
                sawEncrypted = sawEncrypted || found.contains(where: { !$0.isOpen })
                guard let chosen = found.filter(\.isOpen).max(by: { $0.signal < $1.signal }) else { continue }
                if let result = gate() { return result }
                guard authorization.isValid else { return .cancelled }
                try radio.associateOpen(chosen)
                return authorization.isValid ? .joined : .cancelled
            } catch {
                // One failed target must not starve the other remembered networks. Recheck
                // cancellation and physical state before permitting another directed attempt.
                if let result = gate() { return result }
                sawFailure = true
            }
        }
        return sawFailure ? .failed : (sawEncrypted ? .systemConnectionRequired : .notFound)
    }
}

/// No system preferences enumeration and no keychain reads. Only open Wi-Fi can be associated.
final class CoreWLANRecoveryRadio: WiFiRecoveryRadio {
    private var cache: [UUID: CWNetwork] = [:]
    func permissionGranted() -> Bool {
        // Read the modern instance property on the main run loop. This temporary manager has
        // no delegate and never requests authorization or starts location updates.
        let read = { CLLocationManager().authorizationStatus == .authorizedAlways }
        return Thread.isMainThread ? read() : DispatchQueue.main.sync(execute: read)
    }
    func powerState() -> Bool? { CWWiFiClient.shared().interface()?.powerOn() }
    func setPowerOn() throws {
        guard let interface = CWWiFiClient.shared().interface() else { throw CocoaError(.featureUnsupported) }
        try interface.setPower(true)
    }
    func scan(ssid: String) throws -> [WiFiScanCandidate] {
        cache.removeAll()
        guard let interface = CWWiFiClient.shared().interface() else { throw CocoaError(.featureUnsupported) }
        return try interface.scanForNetworks(withName: ssid).compactMap { network in
            guard network.ssid == ssid else { return nil }
            let id = UUID(); cache[id] = network
            return WiFiScanCandidate(id: id, ssid: ssid, signal: network.rssiValue,
                                     isOpen: network.supportsSecurity(.none))
        }
    }
    func associateOpen(_ candidate: WiFiScanCandidate) throws {
        guard candidate.isOpen, let network = cache[candidate.id], network.ssid == candidate.ssid,
              network.supportsSecurity(.none), let interface = CWWiFiClient.shared().interface() else {
            throw CocoaError(.featureUnsupported)
        }
        // CWInterface.h requires passphrases for personal security. Never assume nil reuses saved secrets.
        try interface.associate(to: network, password: nil)
        cache.removeAll()
    }

    /// Empty/redacted SSID is never used as evidence of being disconnected.
    static func classifyLink(powered: Bool, visibleAssociation: Bool, linkActive: Bool?,
                             hasAddress: Bool, interfaceRunning: Bool?, serviceActive: Bool) -> WiFiPhysicalState {
        if visibleAssociation || linkActive == true { return .connected }
        if !powered || linkActive == false || interfaceRunning == false { return .disconnected }
        if hasAddress { return .connected }
        // serviceActive is a service state, not proof of association or disconnection.
        _ = serviceActive
        return .unknown
    }

    /// Link-local and loopback addresses survive some link transitions; they cannot prove an active network.
    static func usableAddress(_ text: String) -> Bool {
        var v4 = in_addr()
        if inet_pton(AF_INET, text, &v4) == 1 {
            let octets = withUnsafeBytes(of: v4.s_addr) { Array($0) }
            return octets[0] != 0 && octets[0] != 127 && octets[0] < 224
                && !(octets[0] == 169 && octets[1] == 254)
        }
        var v6 = in6_addr()
        guard inet_pton(AF_INET6, text, &v6) == 1 else { return false }
        let bytes = withUnsafeBytes(of: v6) { Array($0) }
        guard bytes.count == 16 else { return false }
        return !bytes.allSatisfy({ $0 == 0 }) && !(bytes.prefix(15).allSatisfy({ $0 == 0 }) && bytes[15] == 1)
            && !(bytes[0] == 0xfe && bytes[1] & 0xc0 == 0x80) && bytes[0] != 0xff
    }

    private static func addressText(_ address: UnsafePointer<sockaddr>) -> String? {
        let family = Int32(address.pointee.sa_family)
        var buffer = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
        let converted: Bool
        if family == AF_INET {
            var bytes = UnsafeRawPointer(address).assumingMemoryBound(to: sockaddr_in.self).pointee.sin_addr
            converted = inet_ntop(AF_INET, &bytes, &buffer, socklen_t(buffer.count)) != nil
        } else if family == AF_INET6 {
            var bytes = UnsafeRawPointer(address).assumingMemoryBound(to: sockaddr_in6.self).pointee.sin6_addr
            converted = inet_ntop(AF_INET6, &bytes, &buffer, socklen_t(buffer.count)) != nil
        } else { return nil }
        return converted ? String(cString: buffer) : nil
    }

    func physicalState() -> WiFiPhysicalState {
        guard let store = SCDynamicStoreCreate(nil, "local.SZUConnect.wifi-gate" as CFString, nil, nil),
              let interfaces = SCNetworkInterfaceCopyAll() as? [SCNetworkInterface] else { return .unknown }
        let wifi = CWWiFiClient.shared().interfaces() ?? []
        var flagsByName: [String: UInt32] = [:]
        var addresses: Set<String> = []
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0 else { return .unknown }
        defer { if let head { freeifaddrs(head) } }
        var current = head
        while let pointer = current {
            let item = pointer.pointee; current = item.ifa_next
            let name = String(cString: item.ifa_name)
            flagsByName[name] = item.ifa_flags
            if let address = item.ifa_addr, let text = Self.addressText(address), Self.usableAddress(text),
               item.ifa_flags & UInt32(IFF_UP) != 0 { addresses.insert(name) }
        }
        var observed = 0
        var uncertain = false
        for interface in interfaces {
            guard let rawType = SCNetworkInterfaceGetInterfaceType(interface),
                  rawType == kSCNetworkInterfaceTypeEthernet || rawType == kSCNetworkInterfaceTypeIEEE80211,
                  let rawName = SCNetworkInterfaceGetBSDName(interface) else { continue }
            let name = rawName as String
            let wireless = wifi.first { $0.interfaceName == name }
            let link = SCDynamicStoreCopyValue(store, "State:/Network/Interface/\(name)/Link" as CFString) as? [String: Any]
            let flags = flagsByName[name]
            // Configured but absent wired adapters are not live interfaces and cannot block recovery.
            if flags == nil && link == nil && wireless == nil { continue }
            let state = Self.classifyLink(powered: wireless?.powerOn() ?? true,
                visibleAssociation: !(wireless?.ssid() ?? "").isEmpty || !(wireless?.bssid() ?? "").isEmpty,
                linkActive: link?["Active"] as? Bool, hasAddress: addresses.contains(name),
                interfaceRunning: flags.map { $0 & UInt32(IFF_RUNNING) != 0 }, serviceActive: wireless?.serviceActive() ?? false)
            observed += 1
            if state == .connected { return .connected }
            uncertain = uncertain || state == .unknown
        }
        return observed == 0 || uncertain ? .unknown : .disconnected
    }
}
#endif
