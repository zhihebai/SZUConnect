import Foundation

@main @MainActor
struct DockPreferencesAppModelTests {
    static func main() {
        var checks = 0
        func expect(_ value: Bool, _ reason: String) {
            precondition(value, reason)
            checks += 1
            print("PASS: " + reason)
        }
        let domain = "local.SZUConnect.offline-dock.\(UUID().uuidString)"
        let preferences = UserDefaults(suiteName: domain)!
        defer { preferences.removePersistentDomain(forName: domain) }
        preferences.set("fixture-other-setting", forKey: "fixtureExistingValue")
        let app = AppModel(isolated: true, isolatedPreferences: preferences)
        expect(!app.alwaysShowDockIcon && app.dockPreferenceAvailable, "Older settings without the new field retain default window-following behavior")
        expect(preferences.object(forKey: "alwaysShowDockIcon") == nil, "Loading older preferences does not create or rewrite a field")
        app.setAlwaysShowDockIcon(true)
        expect(app.alwaysShowDockIcon && preferences.bool(forKey: "alwaysShowDockIcon"), "A user choice is immediately reflected and saved")
        let reopened = AppModel(isolated: true, isolatedPreferences: preferences)
        expect(reopened.alwaysShowDockIcon, "A new model restores the saved choice")
        let before = preferences.persistentDomain(forName: domain) as NSDictionary?
        let readOnly = AppModel(isolated: true, readOnly: true, isolatedPreferences: preferences)
        readOnly.setAlwaysShowDockIcon(false)
        expect(readOnly.alwaysShowDockIcon && preferences.persistentDomain(forName: domain) as NSDictionary? == before,
               "Read-only display loads the choice but cannot change the stored value")
        reopened.setAlwaysShowDockIcon(false)
        expect(!reopened.alwaysShowDockIcon && !preferences.bool(forKey: "alwaysShowDockIcon"), "Disabling the setting is persisted")
        expect(preferences.string(forKey: "fixtureExistingValue") == "fixture-other-setting", "Existing unrelated settings remain intact")
        preferences.set("fixture-unknown-future-format", forKey: "alwaysShowDockIcon")
        let unsupported = AppModel(isolated: true, isolatedPreferences: preferences)
        unsupported.setAlwaysShowDockIcon(true)
        expect(!unsupported.dockPreferenceAvailable && preferences.string(forKey: "alwaysShowDockIcon") == "fixture-unknown-future-format",
               "An unknown value is preserved rather than rewritten as a default")
        print("Dock preference contracts: \(checks) passed")
    }
}
