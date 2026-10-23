import Foundation

/// Every preference read and write stays in RAM; no persistent preference domain is accessed.
private final class MemoryDefaults: UserDefaults, @unchecked Sendable {
    var values: [String: Any]
    var writtenKeys: [String] = []
    init(_ values: [String: Any] = [:]) {
        self.values = values
        super.init(suiteName: "SZUConnect.TourTests.InMemory")!
    }
    override func object(forKey key: String) -> Any? { values[key] }
    override func string(forKey key: String) -> String? { values[key] as? String }
    override func set(_ value: Any?, forKey key: String) {
        writtenKeys.append(key)
        values[key] = value
    }
}

private struct CheckFailure: Error { let name: String }

@main
private struct TourStateTests {
    static var checks = 0
    static func check(_ value: @autoclosure () -> Bool, _ name: String) throws {
        guard value() else { throw CheckFailure(name: name) }
        checks += 1
    }
    static func main() {
        do {
            try progressBoundaries()
            try freshDetection()
            try legacyAndPersistence()
            try connectionSetupProgress()
            try upgradeKeepsTutorialChoices()
            print("Tour state tests: \(checks) checks, 0 failures")
        } catch let failure as CheckFailure {
            print("FAIL: \(failure.name)")
            exit(1)
        } catch {
            print("FAIL: unexpected test error")
            exit(1)
        }
    }

    static func progressBoundaries() throws {
        let start = Date(timeIntervalSince1970: 100)
        var progress = TourProgress(now: start)
        try check(progress.index == 0 && progress.step == .welcome && !progress.isLast,
                  "A new guide starts at welcome")
        progress.move(by: -1, now: start.addingTimeInterval(1))
        try check(progress.index == 0 && progress.step == .welcome, "Back at start is bounded")
        progress.move(by: Int.max, now: start.addingTimeInterval(2))
        try check(progress.isLast && progress.step == .finish, "Large forward jump from start is bounded")
        progress.move(by: Int.min, now: start.addingTimeInterval(3))
        try check(progress.index == 0, "Large backward jump is bounded")
        for _ in 0..<30 { progress.move(by: 1) }
        try check(progress.isLast && progress.step == .finish, "Repeated next cannot exceed final step")
        for _ in 0..<30 { progress.move(by: -1) }
        try check(progress.index == 0 && progress.step == .welcome, "Repeated back cannot precede welcome")
        progress.move(by: 1)
        progress.move(by: Int.max)
        try check(progress.isLast, "Huge forward offset from a nonzero index does not overflow")
        progress.move(by: Int.min)
        try check(progress.index == 0, "Huge backward offset remains bounded")
        let empty = TourProgress(steps: [], now: start)
        try check(empty.steps == [.welcome, .finish] && !empty.isLast, "Empty custom step list has a safe fallback")
        var single = TourProgress(steps: [.account], now: start)
        try check(single.isLast && single.step == .account, "Single step is immediately final")
        single.move(by: -4)
        single.move(by: 4)
        try check(single.index == 0, "Single step remains bounded in both directions")
        let settingsSteps = TourStep.available.filter(\.usesSettings)
        #if os(macOS)
        try check(settingsSteps == [.startup, .wifi], "Only available macOS settings steps route to settings")
        #else
        try check(settingsSteps.isEmpty, "Non-macOS step list excludes unsupported system settings")
        #endif
        try check(TourStep.detection.target == .status && TourStep.login.target == .status,
                  "Check and optional login explain the status control")
        try check(TourStep.welcome.target == nil && TourStep.finish.target == nil,
                  "Welcome and finish require no control")
    }

    static func freshDetection() throws {
        let entered = Date(timeIntervalSince1970: 200)
        var progress = TourProgress(steps: [.detection, .account], now: entered)
        try check(!progress.hasFreshCheck(completedAt: nil, busy: false), "No result is not a completed check")
        try check(!progress.hasFreshCheck(completedAt: entered.addingTimeInterval(-1), busy: false),
                  "An old result cannot complete this step")
        try check(!progress.hasFreshCheck(completedAt: entered, busy: true), "Busy operation cannot be treated as finished")
        try check(progress.hasFreshCheck(completedAt: entered, busy: false), "Completion at entry boundary counts")
        try check(progress.hasFreshCheck(completedAt: entered.addingTimeInterval(1), busy: false), "New completed result counts")
        progress.move(by: 1, now: entered.addingTimeInterval(10))
        progress.move(by: -1, now: entered.addingTimeInterval(20))
        try check(!progress.hasFreshCheck(completedAt: entered.addingTimeInterval(1), busy: false),
                  "Returning to detection does not reuse its previous result")
        try check(progress.hasFreshCheck(completedAt: entered.addingTimeInterval(21), busy: false),
                  "Returning step accepts a new completed result")
    }

    static func legacyAndPersistence() throws {
        let defaults = MemoryDefaults()
        try check(TutorialState.status(defaults) == .none, "Missing state is unread")
        try check(TutorialState.shouldPresentAutomatically(defaults: defaults, legacy: false), "Fresh user is eligible")
        try check(!TutorialState.shouldPresentAutomatically(defaults: defaults, legacy: true), "Legacy user is never forced")
        try check(!TutorialState.isLegacyUser(defaults: defaults, hasSavedCredentials: false, hasHistory: false),
                  "Empty preferences alone do not make a legacy user")
        for key in ["area", "monitoringMode", "autoReconnect", "appearanceMode", "probeMode"] {
            let existing = MemoryDefaults([key: false])
            try check(TutorialState.isLegacyUser(defaults: existing, hasSavedCredentials: false, hasHistory: false),
                      "Existing \(key), even false, preserves legacy classification")
        }
        try check(TutorialState.isLegacyUser(defaults: nil, hasSavedCredentials: true, hasHistory: false),
                  "Saved credentials alone imply an existing user")
        try check(TutorialState.isLegacyUser(defaults: nil, hasSavedCredentials: false, hasHistory: true),
                  "History alone implies an existing user")
        let unrelated = MemoryDefaults(["unrelated": true])
        try check(!TutorialState.isLegacyUser(defaults: unrelated, hasSavedCredentials: false, hasHistory: false),
                  "Unrelated keys do not incorrectly suppress first-use guide")
        defaults.values[TutorialState.statusKey] = "unknown-state"
        try check(TutorialState.status(defaults) == .none, "Unknown tutorial value has a bounded fallback")
        for status in [TutorialState.Status.skipped, .completed] {
            TutorialState.setStatus(status, defaults)
            try check(TutorialState.status(defaults) == status, "Explicit exit status round trips")
            try check(!TutorialState.shouldPresentAutomatically(defaults: defaults, legacy: false), "Exited guide does not force a replay")
            try check(!TutorialState.shouldShowWhatsNew(defaults: defaults, legacy: true), "Exited guide does not force an upgrade card")
        }
        let old = MemoryDefaults([TutorialState.whatsNewKey: "0.7.1", "area": "teaching", "autoReconnect": false])
        try check(TutorialState.shouldShowWhatsNew(defaults: old, legacy: true), "Old dismissed version can show this optional upgrade card")
        TutorialState.dismissWhatsNew(old)
        try check(!TutorialState.shouldShowWhatsNew(defaults: old, legacy: true), "Current dismissal survives another presentation decision")
        try check(old.values["area"] as? String == "teaching" && old.values["autoReconnect"] as? Bool == false,
                  "Dismissing tutorial card does not alter formal preference values")
        try check(Set(old.writtenKeys) == [TutorialState.whatsNewKey], "Dismiss writes only tutorial metadata")
        TutorialState.setStatus(.completed, old)
        try check(Set(old.writtenKeys) == [TutorialState.statusKey, TutorialState.whatsNewKey],
                  "Completion writes only tutorial metadata")
        TutorialState.setStatus(.skipped, nil)
        TutorialState.dismissWhatsNew(nil)
        try check(TutorialState.status(nil) == .none, "Nil tutorial storage remains stateless")
    }

    static func connectionSetupProgress() throws {
        try check(ConnectionSetupStep.next(networkAvailable: false, hasNetworkMemory: false,
                                           accountSaved: false, automatic: false) == .network,
                  "Fresh offline install first explains how to join the campus link")
        try check(ConnectionSetupStep.next(networkAvailable: true, hasNetworkMemory: false,
                                           accountSaved: false, automatic: false) == .memory,
                  "A usable link needs explicit campus memory before automatic setup proceeds")
        try check(ConnectionSetupStep.next(networkAvailable: true, hasNetworkMemory: false,
                                           accountSaved: true, automatic: true) == .memory,
                  "Saved credentials and automatic mode never substitute for remembering a new network")
        try check(ConnectionSetupStep.next(networkAvailable: true, hasNetworkMemory: true,
                                           accountSaved: false, automatic: false) == .account,
                  "After remembering, missing saved credentials precede enabling recovery")
        try check(ConnectionSetupStep.next(networkAvailable: true, hasNetworkMemory: true,
                                           accountSaved: true, automatic: false) == .recovery,
                  "Saved network and credentials leave the explicit recovery choice")
        try check(ConnectionSetupStep.next(networkAvailable: true, hasNetworkMemory: true,
                                           accountSaved: true, automatic: true) == .ready,
                  "Complete basic setup is ready without optional Wi-Fi power or association settings")
        try check(ConnectionSetupStep.next(networkAvailable: false, hasNetworkMemory: true,
                                           accountSaved: true, automatic: true) == .ready,
                  "A later disconnection does not send a configured user through first-use setup again")
    }

    static func upgradeKeepsTutorialChoices() throws {
        let completed = MemoryDefaults(["tutorial.status": "completed", "monitoringMode": "recover"])
        try check(TutorialState.status(completed) == .completed,
                  "Stored pre-upgrade tutorial completion keeps its original key and value")
        try check(!TutorialState.shouldPresentAutomatically(defaults: completed, legacy: true)
                    && !TutorialState.shouldShowWhatsNew(defaults: completed, legacy: true),
                  "Upgrading a completed tutorial never forces a replay or introduction card")
        let dismissed = MemoryDefaults(["tutorial.whatsNewDismissed": "0.9.0", "area": "automatic"])
        try check(!TutorialState.shouldPresentAutomatically(defaults: dismissed, legacy: true)
                    && !TutorialState.shouldShowWhatsNew(defaults: dismissed, legacy: true),
                  "The existing introduction dismissal remains honored by the setup-card upgrade")
        try check(completed.writtenKeys.isEmpty && dismissed.writtenKeys.isEmpty,
                  "Upgrade presentation decisions do not rewrite tutorial or connection preferences")
    }
}
