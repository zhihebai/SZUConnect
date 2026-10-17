#if os(macOS)
import AppKit

/// Window ownership is separate from visibility: minimizing or hiding an app does not close it.
struct DockWindowState {
    var isOpen: Bool
    var isVisible: Bool
    var isMiniaturized: Bool
    var appIsHidden: Bool

    var needsDock: Bool { isOpen && (isVisible || isMiniaturized || appIsHidden) }
}

enum DockVisibilityDecision {
    static func policy(alwaysShow: Bool, windows: [DockWindowState], interacting: Bool) -> NSApplication.ActivationPolicy {
        alwaysShow || interacting || windows.contains(where: \.needsDock) ? .regular : .accessory
    }
}

/// Owns activation policy only. Background network and model updates never activate a window.
@MainActor
final class DockVisibilityController {
    private final class TrackedWindow {
        weak var window: NSWindow?
        var open = true
        init(_ window: NSWindow) { self.window = window }
    }

    private let application: NSApplication
    private var tracked: [ObjectIdentifier: TrackedWindow] = [:]
    private var observers: [NSObjectProtocol] = []
    private var permissionOwners = Set<ObjectIdentifier>()
    private var scheduled = false
    private var started = false
    private var startupSettled = false
    private var presentingUserWindow = false
    private(set) var alwaysShow = false
    var onFailure: (() -> Void)?

    init(application: NSApplication? = nil) {
        self.application = application ?? .shared
    }

    func start(alwaysShow: Bool) {
        self.alwaysShow = alwaysShow
        guard !started else { synchronize(); return }
        started = true
        let center = NotificationCenter.default
        for name in [NSWindow.didBecomeKeyNotification, NSWindow.didBecomeMainNotification,
                     NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification,
                     NSWindow.willBeginSheetNotification, NSWindow.didEndSheetNotification,
                     NSWindow.didUpdateNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    if let window = note.object as? NSWindow, self.isUserWindow(window),
                       window.isVisible || window.isMiniaturized {
                        let visibleReopen = note.name == NSWindow.didBecomeKeyNotification
                            || note.name == NSWindow.didBecomeMainNotification
                            || note.name == NSWindow.didDeminiaturizeNotification
                        if self.tracked[ObjectIdentifier(window)] == nil || visibleReopen { self.track(window) }
                        if window.identifier?.rawValue == "SZUConnect.dashboard" {
                            self.presentingUserWindow = false
                        }
                    }
                    self.scheduleSynchronization()
                }
            })
        }
        observers.append(center.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { [weak self] note in
            MainActor.assumeIsolated {
                guard let self, let window = note.object as? NSWindow else { return }
                self.tracked[ObjectIdentifier(window)]?.open = false
                if window.identifier?.rawValue == "SZUConnect.dashboard" { self.presentingUserWindow = false }
                self.scheduleSynchronization()
            }
        })
        for name in [NSApplication.didHideNotification, NSApplication.didUnhideNotification,
                     NSApplication.didBecomeActiveNotification, NSApplication.didResignActiveNotification] {
            observers.append(center.addObserver(forName: name, object: application, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.scheduleSynchronization() }
            })
        }
        observers.append(center.addObserver(forName: Notification.Name("SZUConnect.permissionInteraction"), object: nil, queue: .main) { [weak self] note in
            MainActor.assumeIsolated {
                guard let self, let owner = note.object as AnyObject?, let active = note.userInfo?["active"] as? Bool else { return }
                let id = ObjectIdentifier(owner)
                if active { self.permissionOwners.insert(id) } else { self.permissionOwners.remove(id) }
                self.scheduleSynchronization()
            }
        })
        synchronize()
    }

    func setAlwaysShow(_ enabled: Bool) {
        guard alwaysShow != enabled else { return }
        alwaysShow = enabled
        synchronize()
    }

    func finishStartup() {
        startupSettled = true
        synchronize()
    }

    func track(_ window: NSWindow) {
        let id = ObjectIdentifier(window)
        if let prior = tracked[id], prior.window === window { prior.open = true }
        else { tracked[id] = TrackedWindow(window) }
    }

    /// Explicit user actions may reveal a window; routine state refreshes cannot call this.
    func prepareToPresent(_ window: NSWindow? = nil) {
        presentingUserWindow = true
        if let window { track(window) }
        apply(.regular)
    }

    func backgroundLaunchWindow(_ window: NSWindow) {
        track(window)
        tracked[ObjectIdentifier(window)]?.open = false
        window.orderOut(nil)
        window.alphaValue = 1
        scheduleSynchronization()
    }

    func scheduleSynchronization() {
        guard !scheduled else { return }
        scheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.scheduled = false
            self.synchronize()
        }
    }

    func synchronize() {
        guard started, startupSettled else { return }
        tracked = tracked.filter { $0.value.window != nil }
        // Retain auxiliary dialogs, but exclude status-item panels and AppKit's menu windows.
        for window in application.windows where isUserWindow(window) && window.isVisible {
            if tracked[ObjectIdentifier(window)] == nil { track(window) }
        }
        let windows = tracked.values.compactMap { item -> DockWindowState? in
            guard let window = item.window else { return nil }
            return DockWindowState(isOpen: item.open, isVisible: window.isVisible,
                                   isMiniaturized: window.isMiniaturized, appIsHidden: application.isHidden)
        }
        let sheetActive = tracked.values.contains { $0.open && $0.window?.attachedSheet != nil }
        apply(DockVisibilityDecision.policy(alwaysShow: alwaysShow, windows: windows,
              interacting: application.modalWindow != nil || sheetActive || !permissionOwners.isEmpty || presentingUserWindow))
    }

    private func isUserWindow(_ window: NSWindow) -> Bool {
        if window.identifier?.rawValue == "SZUConnect.dashboard" { return true }
        let className = NSStringFromClass(type(of: window))
        return window.canBecomeKey && window.level.rawValue < NSWindow.Level.statusBar.rawValue
            && !window.styleMask.contains(.nonactivatingPanel)
            && !className.contains("MenuBarExtra") && !className.contains("NSStatusBar")
            && !className.contains("NSMenu")
    }

    private func apply(_ desired: NSApplication.ActivationPolicy) {
        guard application.activationPolicy() != desired else { return }
        let accepted = application.setActivationPolicy(desired)
        guard accepted, application.activationPolicy() == desired else {
            // Keep the current policy and the independent menu-bar entry available.
            onFailure?()
            return
        }
    }

    deinit {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
    }
}
#endif
