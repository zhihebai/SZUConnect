import Foundation

/// Skippable tutorial state. Opening it never authenticates, trusts a network or
/// enables launch at login; those happen only through the regular controls it points to.
enum TutorialState {
    static let statusKey = "tutorial.status"
    static let whatsNewKey = "tutorial.whatsNewDismissed"
    // Keep the original introduction dismissal marker across upgrades. The live
    // checklist reflects missing setup without reopening a completed tutorial.
    static let currentVersion = "0.9.0"
    enum Status: String { case none, skipped, completed }

    static func status(_ defaults: UserDefaults?) -> Status {
        Status(rawValue: defaults?.string(forKey: statusKey) ?? "") ?? .none
    }
    static func setStatus(_ status: Status, _ defaults: UserDefaults?) { defaults?.set(status.rawValue, forKey: statusKey) }

    /// Existing preferences, saved credentials or history suppress automatic tutorial presentation.
    /// An optional introduction remains dismissible.
    static func isLegacyUser(defaults: UserDefaults?, hasSavedCredentials: Bool, hasHistory: Bool) -> Bool {
        let legacyKeys = ["area", "monitoringMode", "autoReconnect", "appearanceMode", "probeMode"]
        return hasSavedCredentials || hasHistory || legacyKeys.contains { defaults?.object(forKey: $0) != nil }
    }

    static func shouldPresentAutomatically(defaults: UserDefaults?, legacy: Bool) -> Bool {
        status(defaults) == .none && !legacy
    }

    static func shouldShowWhatsNew(defaults: UserDefaults?, legacy: Bool) -> Bool {
        legacy && status(defaults) == .none && defaults?.string(forKey: whatsNewKey) != currentVersion
    }

    static func dismissWhatsNew(_ defaults: UserDefaults?) { defaults?.set(currentVersion, forKey: whatsNewKey) }
}

/// Navigation and progress only. The guide has no connection service or credential access.
enum TourTarget: Hashable {
    case status, account, recovery, websites, network, startup, wifiRecovery, help
}

/// A view-only checklist. Existing preferences determine progress; visiting it grants no permission.
enum ConnectionSetupStep: String {
    case network, memory, account, recovery, ready

    static func next(networkAvailable: Bool, hasNetworkMemory: Bool, accountSaved: Bool, automatic: Bool) -> Self {
        if !hasNetworkMemory { return networkAvailable ? .memory : .network }
        if !accountSaved { return .account }
        if !automatic { return .recovery }
        return .ready
    }
}

enum TourStep: String, CaseIterable, Identifiable {
    case welcome, detection, account, login, recovery, memory, websites, network, startup, wifi, finish
    var id: String { rawValue }
    var target: TourTarget? {
        switch self {
        case .welcome, .finish: return nil
        case .detection, .login: return .status
        case .account: return .account
        case .recovery, .memory: return .recovery
        case .websites: return .websites
        case .network: return .network
        case .startup: return .startup
        case .wifi: return .wifiRecovery
        }
    }
    var section: String {
        switch self {
        case .recovery, .memory: return "guard"
        case .detection, .login, .welcome, .finish: return "status"
        case .wifi: return "wifiRecovery"
        default: return rawValue
        }
    }
    var usesSettings: Bool { self == .startup || self == .wifi }
    static var available: [TourStep] {
        #if os(macOS)
        return allCases
        #else
        return allCases.filter { $0 != .startup && $0 != .wifi }
        #endif
    }
}

struct TourProgress {
    let steps: [TourStep]
    private(set) var index = 0
    private(set) var enteredAt: Date
    var step: TourStep { steps[index] }
    var isLast: Bool { index == steps.count - 1 }
    init(steps: [TourStep] = TourStep.available, now: Date = Date()) {
        self.steps = steps.isEmpty ? [.welcome, .finish] : steps
        enteredAt = now
    }
    mutating func move(by offset: Int, now: Date = Date()) {
        index += min(max(offset, -index), steps.count - 1 - index)
        enteredAt = now
    }
    func hasFreshCheck(completedAt: Date?, busy: Bool) -> Bool {
        guard !busy, let completedAt else { return false }
        return completedAt >= enteredAt
    }
}
