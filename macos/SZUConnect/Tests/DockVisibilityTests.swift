import AppKit

@main
struct DockVisibilityTests {
    static func main() {
        var count = 0
        func expect(_ value: NSApplication.ActivationPolicy, _ wanted: NSApplication.ActivationPolicy, _ message: String) {
            precondition(value == wanted, message)
            count += 1
            print("PASS: " + message)
        }
        func policy(_ windows: [DockWindowState] = [], always: Bool = false, interaction: Bool = false) -> NSApplication.ActivationPolicy {
            DockVisibilityDecision.policy(alwaysShow: always, windows: windows, interacting: interaction)
        }
        let open = DockWindowState(isOpen: true, isVisible: true, isMiniaturized: false, appIsHidden: false)
        let minimized = DockWindowState(isOpen: true, isVisible: false, isMiniaturized: true, appIsHidden: false)
        let hidden = DockWindowState(isOpen: true, isVisible: false, isMiniaturized: false, appIsHidden: true)
        let closed = DockWindowState(isOpen: false, isVisible: true, isMiniaturized: false, appIsHidden: true)
        expect(policy(), .accessory, "Default background launch needs no Dock tile")
        expect(policy([open]), .regular, "A user window retains its Dock tile")
        expect(policy([minimized]), .regular, "Minimizing is not closing")
        expect(policy([hidden]), .regular, "Hiding is not closing")
        expect(policy([closed]), .accessory, "A closing window cannot be revived by a stale visible or hidden flag")
        expect(policy([closed, open]), .regular, "Closing the dashboard preserves a separate user dialog")
        expect(policy([closed], interaction: true), .regular, "An active system interaction defers switching to background")
        expect(policy([closed]), .accessory, "Finishing the interaction returns to the actual window state")
        expect(policy(always: true), .regular, "Always-show applies without an open window")
        expect(policy([closed], always: true), .regular, "Always-show survives the final window closing")
        expect(policy([open], always: false), .regular, "Disabling always-show retains an open window")
        expect(policy([closed], always: false), .accessory, "Disabling always-show with no window restores background mode")
        print("Dock visibility contracts: \(count) passed")
    }
}
