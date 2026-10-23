import AppKit

@MainActor private final class LifecycleCloseVeto: NSObject, NSWindowDelegate {
    private(set) var userCloseRequests = 0
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        userCloseRequests += 1
        return false
    }
}

@main @MainActor
struct AppUninstallLifecycleTests {
    static func main() {
        setbuf(stdout, nil)
        let application = NSApplication.shared
        application.setActivationPolicy(.accessory)
        application.finishLaunching()
        var checks = 0
        func expect(_ condition: Bool, _ message: String) {
            precondition(condition, "FAIL: " + message)
            checks += 1; print("PASS: " + message)
        }
        func settle(until condition: () -> Bool) {
            let deadline = Date(timeIntervalSinceNow: 3)
            while !condition(), Date() < deadline {
                RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            expect(condition(), "Deferred lifecycle action finishes without blocking the main run loop")
        }
        func window(_ identifier: String) -> NSWindow {
            let value = NSWindow(contentRect: NSRect(x: -4000, y: -4000, width: 320, height: 180),
                styleMask: [.titled, .closable], backing: .buffered, defer: false)
            value.identifier = NSUserInterfaceItemIdentifier(identifier)
            value.isReleasedWhenClosed = false
            value.alphaValue = 0
            value.animationBehavior = .none
            return value
        }

        do {
            let dashboard = window("fixture.dashboard")
            let about = window("fixture.about")
            let confirmation = window("fixture.confirmation")
            let veto = LifecycleCloseVeto()
            about.delegate = veto; confirmation.delegate = veto
            var completions: [String] = [], closures: [String] = []
            let observer = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification,
                object: nil, queue: .main) { note in
                    MainActor.assumeIsolated {
                        if let window = note.object as? NSWindow,
                           let identifier = window.identifier?.rawValue, identifier.hasPrefix("fixture.") {
                            closures.append(identifier)
                        }
                    }
                }
            defer { NotificationCenter.default.removeObserver(observer) }
            dashboard.beginSheet(about) { _ in completions.append("about") }
            about.beginSheet(confirmation) { _ in completions.append("confirmation") }
            expect(dashboard.attachedSheet === about && about.attachedSheet === confirmation,
                "The fixture creates real nested AppKit sheet relationships")
            // A real sheet is ordered by AppKit, and orderOut would end its sheet
            // relationship. Alpha was set before beginSheet so the real fixture
            // remains completely transparent throughout this regression.
            expect(dashboard.alphaValue == 0 && about.alphaValue == 0 && confirmation.alphaValue == 0,
                "Nested lifecycle fixtures remain transparent and never expose user data")

            var terminations = 0, endedBeforeTermination = false, closedBeforeTermination = false
            let lifecycle = AppUninstallLifecycle(application: application) {
                terminations += 1
                endedBeforeTermination = dashboard.attachedSheet == nil && about.attachedSheet == nil
                    && about.sheetParent == nil && confirmation.sheetParent == nil
                closedBeforeTermination = closures.contains("fixture.dashboard") && closures.contains("fixture.about")
                    && closures.contains("fixture.confirmation")
            }
            lifecycle.finish(); lifecycle.finish()
            expect(lifecycle.isFinishing && !lifecycle.terminationRequested && terminations == 0,
                "Finish returns before termination and coalesces repeated success callbacks")
            settle { terminations == 1 }
            expect(lifecycle.terminationRequested && endedBeforeTermination && closedBeforeTermination,
                "Termination is requested only after nested sheet sessions end and frozen windows close")
            expect(Array(closures.prefix(3)) == ["fixture.confirmation", "fixture.about", "fixture.dashboard"],
                "Confirmation closes before About, and About closes before the dashboard")
            expect(veto.userCloseRequests == 0,
                "Successful shutdown bypasses an interactive sheet-close veto without simulating a user action")
            settle { completions.count == 2 }
            expect(completions.first == "confirmation" && completions.last == "about",
                "Both real sheet completion handlers run in child-first order")
            lifecycle.finish()
            RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.03))
            expect(terminations == 1, "A later repeated finish cannot request termination twice")
            expect(!dashboard.isVisible && !about.isVisible && !confirmation.isVisible,
                "Shutdown never reveals a hidden fixture window")
        }

        do {
            let hidden = window("fixture.no-sheet")
            var terminations = 0
            let lifecycle = AppUninstallLifecycle(application: application) { terminations += 1 }
            lifecycle.finish()
            expect(terminations == 0, "An already closed or sheet-free application still defers termination")
            settle { terminations == 1 }
            expect(!hidden.isVisible && lifecycle.terminationRequested,
                "The sheet-free path completes without creating any application window")
        }
        print("App uninstall lifecycle: \(checks) assertions passed; hidden windows and a termination spy only")
    }
}
