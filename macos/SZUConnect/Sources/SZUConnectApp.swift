import SwiftUI
#if os(macOS)
import AppKit
import Combine
import Carbon
#endif

@main struct SZUConnectApp: App {
    @StateObject private var model: AppModel
    @Environment(\.scenePhase) private var phase
    #if os(macOS)
    @NSApplicationDelegateAdaptor(DashboardApplicationDelegate.self) private var appDelegate
    // Acquire before constructing AppModel, including --observe-only launches.
    private let instanceLock: SingleInstanceLock
    #endif

    init() {
        #if os(macOS)
        AppStartupSignals.configure()
        instanceLock = Self.acquireInstanceOrExit()
        #endif
        let initialModel = AppModel()
        _model = StateObject(wrappedValue: initialModel)
        #if os(macOS)
        DashboardApplicationDelegate.prepare(model: initialModel)
        #endif
    }

    #if os(macOS)
    private static func acquireInstanceOrExit() -> SingleInstanceLock {
        guard let identifier = Bundle.main.bundleIdentifier,
              let cache = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first else {
            startupFailure()
        }
        do {
            if let lock = try SingleInstanceLock.acquire(bundleIdentifier: identifier, cacheDirectory: cache) {
                // Older releases do not own this lock. Reuse an already launched copy before
                // initializing a second model; newly starting peers are coordinated by flock.
                if let existing = runningPeer(identifier: identifier), existing.isFinishedLaunching {
                    activateAndExit(existing)
                }
                return lock
            }
        } catch { startupFailure() }
        guard let existing = runningPeer(identifier: identifier) else {
            startupFailure(SharedCopy.t("error.startupBusy"))
        }
        activateAndExit(existing)
    }

    private static func runningPeer(identifier: String) -> NSRunningApplication? {
        NSRunningApplication.runningApplications(withBundleIdentifier: identifier)
            .filter { $0.processIdentifier != ProcessInfo.processInfo.processIdentifier && !$0.isTerminated }
            .sorted { ($0.launchDate ?? .distantFuture) < ($1.launchDate ?? .distantFuture) }.first
    }

    private static func activateAndExit(_ existing: NSRunningApplication) -> Never {
        existing.activate(options: [.activateAllWindows])
        if let url = existing.bundleURL {
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.activates = true
            configuration.createsNewApplicationInstance = false
            NSWorkspace.shared.openApplication(at: url, configuration: configuration) { _, _ in exit(0) }
            // Give Launch Services time to send reopen when the dashboard was closed.
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { exit(0) }
            RunLoop.main.run()
        }
        exit(0)
    }

    private static func startupFailure(_ message: String = SharedCopy.t("error.startupUnavailable")) -> Never {
        let policyAccepted = NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = SharedCopy.t("error.startupTitle")
        alert.informativeText = message
        alert.addButton(withTitle: SharedCopy.t("tray.quit"))
        if !policyAccepted { alert.window.level = .floating }
        alert.runModal()
        exit(1)
    }
    #endif

    var body: some Scene {
        #if os(macOS)
        // Window style shows the same status, actions and guard mode as the main window.
        // Its "打开主窗口" reuses the single dashboard through DashboardApplicationDelegate.
        MenuBarExtra {
            MenuBarPanel().environmentObject(model)
        } label: {
            // Template images (Assets.xcassets/MenuBar-*) follow the menu bar's light/dark rendering.
            Image(model.menuBarImageName).accessibilityLabel(SharedCopy.t("app.name"))
        }
        .menuBarExtraStyle(.window)
        .commands {
            CommandGroup(replacing: .newItem) {}
            DashboardCommands()
        }
        #else
        WindowGroup(SharedCopy.t("app.name"), id: "dashboard") {
            ContentView().environmentObject(model)
                .onChange(of: phase, initial: true) { _, value in model.setForeground(value == .active) }
        }
        #endif
    }
}

#if os(macOS)
@MainActor
final class DashboardApplicationDelegate: NSObject, NSApplicationDelegate {
    static let windowIdentifier = NSUserInterfaceItemIdentifier("SZUConnect.dashboard")
    private static let frameAutosaveName = "SZUConnect.dashboard"
    private static weak var current: DashboardApplicationDelegate?
    private static weak var preparedModel: AppModel?
    private let dockVisibility = DockVisibilityController()
    private let uninstallLifecycle = AppUninstallLifecycle()
    private weak var model: AppModel?
    private var preferenceObservation: AnyCancellable?
    private var dashboardWindow: NSWindow?
    private var lastDashboardFrame: NSRect?
    private var windowCloseObserver: NSObjectProtocol?
    private var finishedLaunching = false
    private var startupHandled = false
    private var backgroundLaunch = false

    override init() {
        super.init()
        Self.current = self
    }

    static func prepare(model: AppModel) { preparedModel = model }

    private func configure() {
        guard let value = Self.preparedModel, model !== value else { return }
        model = value
        dockVisibility.onFailure = { [weak value] in value?.reportDockVisibilityFailure() }
        dockVisibility.start(alwaysShow: value.alwaysShowDockIcon)
        preferenceObservation = value.$alwaysShowDockIcon.removeDuplicates().sink { [weak self] enabled in
            self?.dockVisibility.setAlwaysShow(enabled)
        }
    }

    private func completeStartup() {
        guard finishedLaunching, !startupHandled else { return }
        startupHandled = true
        if !backgroundLaunch { Self.showDashboard() }
        dockVisibility.finishStartup()
    }

    static func showExistingWindow() -> Bool {
        guard let current, current.model?.isMaintaining != true,
              let window = current.dashboardWindow else { return false }
        current.dockVisibility.prepareToPresent(window)
        if window.isMiniaturized { window.deminiaturize(nil) }
        NSApp.unhide(nil)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        current.model?.setForeground(true)
        return true
    }

    static func showDashboard() {
        guard let current else { return }
        current.configure()
        guard let model = current.model, !model.isMaintaining else { return }
        if showExistingWindow() { return }
        current.dockVisibility.prepareToPresent()
        // Own one native window explicitly, including on macOS 14. Menu-bar-only starts
        // never create or momentarily expose a default SwiftUI Window scene.
        let width = max(DashboardLayout.minimumWindowSize.width,
            (Bundle.main.object(forInfoDictionaryKey: "SZUPreviewWidth") as? NSNumber)?.doubleValue
                ?? DashboardLayout.defaultWindowSize.width)
        let height = max(DashboardLayout.minimumWindowSize.height,
            (Bundle.main.object(forInfoDictionaryKey: "SZUPreviewHeight") as? NSNumber)?.doubleValue
                ?? DashboardLayout.defaultWindowSize.height)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: width, height: height),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered, defer: false)
        window.identifier = windowIdentifier
        window.title = SharedCopy.t("app.name")
        window.titleVisibility = .hidden
        window.toolbarStyle = .unified
        window.tabbingMode = .disallowed
        window.isReleasedWhenClosed = false
        window.contentView = DashboardLayout.makeHostingView(ContentView().environmentObject(model))
        current.dashboardWindow = window
        current.dockVisibility.track(window)
        // Restore the frame, not a content size, so the toolbar cannot add height on each reopen.
        let restored = window.setFrameUsingName(Self.frameAutosaveName)
        if let frame = current.lastDashboardFrame {
            DashboardLayout.restoreFrame(frame, in: window)
        } else if restored {
            DashboardLayout.restoreFrame(window.frame, in: window)
        } else { window.center() }
        if !model.sessionReadOnly { _ = window.setFrameAutosaveName(Self.frameAutosaveName) }
        if let observer = current.windowCloseObserver { NotificationCenter.default.removeObserver(observer) }
        current.windowCloseObserver = NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification, object: window, queue: .main) { [weak current, weak window] _ in
                MainActor.assumeIsolated {
                    if let current, let window {
                        current.model?.cancelPendingManualNetworkRead()
                        current.rememberDashboardFrame(window)
                        // A replacement window may reuse the name even before this one deallocates.
                        _ = window.setFrameAutosaveName("")
                    }
                    if current?.dashboardWindow === window { current?.dashboardWindow = nil }
                    if let token = current?.windowCloseObserver {
                        NotificationCenter.default.removeObserver(token)
                        current?.windowCloseObserver = nil
                    }
                }
            }
        window.makeKeyAndOrderFront(nil)
        NSApp.unhide(nil)
        NSApp.activate(ignoringOtherApps: true)
        model.setForeground(true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    private func rememberDashboardFrame(_ window: NSWindow) {
        guard model?.isMaintaining != true else { return }
        lastDashboardFrame = window.frame
        if model?.sessionReadOnly == false { window.saveFrame(usingName: Self.frameAutosaveName) }
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let dashboardWindow { rememberDashboardFrame(dashboardWindow) }
    }

    static func finishAppUninstall() {
        guard let current, current.model?.isMaintaining == true else { return }
        current.uninstallLifecycle.finish()
    }

    static func prepareForAppMaintenance() {
        // NSWindow's own autosave runs independently of AppModel. Remove it before
        // clearing preferences; termination must not recreate the deleted domain.
        if let window = current?.dashboardWindow { _ = window.setFrameAutosaveName("") }
    }

    func applicationWillFinishLaunching(_ notification: Notification) { configure() }
    func applicationDidBecomeActive(_ notification: Notification) { model?.setForeground(true) }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
        let event = NSAppleEventManager.shared().currentAppleEvent
        let loginEvent = event?.paramDescriptor(forKeyword: keyAEPropData)?.enumCodeValue == keyAELaunchedAsLogInItem
            || event?.paramDescriptor(forKeyword: keyAELaunchedAsLogInItem) != nil
        let isolatedBackground = Self.preparedModel?.isPreview == true
            && Bundle.main.object(forInfoDictionaryKey: "SZUPreviewBackgroundLaunch") as? Bool == true
        backgroundLaunch = loginEvent || isolatedBackground
        finishedLaunching = true
        configure()
        completeStartup()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        Self.showDashboard()
        return false
    }
}

/// Bridges the dashboard's stateful navigation into its native toolbar.
struct DashboardToolbarBridge: NSViewRepresentable {
    let toolbarContent: AnyView

    func makeCoordinator() -> Coordinator { Coordinator(content: toolbarContent) }

    func makeNSView(context: Context) -> NSView {
        let view = ToolbarAnchorView()
        view.coordinator = context.coordinator
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        context.coordinator.update(content: toolbarContent)
        if let window = nsView.window { context.coordinator.install(in: window) }
    }

    static func dismantleNSView(_ nsView: NSView, coordinator: Coordinator) {
        coordinator.detach()
    }

    private final class ToolbarAnchorView: NSView {
        weak var coordinator: Coordinator?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let window { coordinator?.install(in: window) }
        }
    }

    @MainActor
    final class Coordinator: NSObject, NSToolbarDelegate {
        private static let itemIdentifier = NSToolbarItem.Identifier("dashboard")
        private let hostingView: NSHostingView<AnyView>
        private let item: NSToolbarItem
        private let toolbar: NSToolbar
        private weak var window: NSWindow?

        init(content: AnyView) {
            hostingView = NSHostingView(rootView: content)
            hostingView.sizingOptions = []
            hostingView.translatesAutoresizingMaskIntoConstraints = false
            let container = NSView(frame: NSRect(x: 0, y: 0,
                width: DashboardLayout.toolbarMinimumWidth, height: DashboardLayout.toolbarHeight))
            container.translatesAutoresizingMaskIntoConstraints = false
            container.addSubview(hostingView)
            NSLayoutConstraint.activate([
                container.widthAnchor.constraint(greaterThanOrEqualToConstant: DashboardLayout.toolbarMinimumWidth),
                // AppKit chooses the width within this range from the available toolbar space.
                container.widthAnchor.constraint(lessThanOrEqualToConstant: 100_000),
                container.heightAnchor.constraint(equalToConstant: DashboardLayout.toolbarHeight),
                hostingView.leadingAnchor.constraint(equalTo: container.leadingAnchor),
                hostingView.trailingAnchor.constraint(equalTo: container.trailingAnchor),
                hostingView.topAnchor.constraint(equalTo: container.topAnchor),
                hostingView.bottomAnchor.constraint(equalTo: container.bottomAnchor)
            ])
            container.setContentHuggingPriority(.defaultLow, for: .horizontal)
            item = NSToolbarItem(itemIdentifier: Self.itemIdentifier)
            item.view = container
            item.label = SharedCopy.t("app.name")
            item.isBordered = false
            item.visibilityPriority = .high
            toolbar = NSToolbar(identifier: "SZUConnect.dashboard.toolbar")
            toolbar.allowsUserCustomization = false
            toolbar.autosavesConfiguration = false
            toolbar.displayMode = .iconOnly
            super.init()
            toolbar.delegate = self
        }

        func update(content: AnyView) {
            hostingView.rootView = content
        }

        func install(in window: NSWindow) {
            if self.window !== window { detach(); self.window = window }
            if window.toolbar !== toolbar { window.toolbar = toolbar }
        }

        func detach() {
            if window?.toolbar === toolbar { window?.toolbar = nil }
            window = nil
        }

        func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
            [Self.itemIdentifier]
        }

        func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
            [Self.itemIdentifier]
        }

        func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier itemIdentifier: NSToolbarItem.Identifier,
                     willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
            itemIdentifier == Self.itemIdentifier ? item : nil
        }
    }
}

struct DashboardCommands: Commands {

    var body: some Commands {
        CommandGroup(before: .windowArrangement) {
            Button(SharedCopy.t("action.showWindow")) {
                DashboardApplicationDelegate.showDashboard()
            }
            .keyboardShortcut("0", modifiers: .command)
        }
    }
}
#endif
