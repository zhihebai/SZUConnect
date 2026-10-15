import SwiftUI
#if os(macOS)
import AppKit
#endif

@main struct SZUConnectApp: App {
    @StateObject private var model: AppModel
    @Environment(\.scenePhase) private var phase
    #if os(macOS)
    @NSApplicationDelegateAdaptor(DashboardApplicationDelegate.self) private var appDelegate
    // Acquire before constructing AppModel, including --observe-only launches.
    private let instanceLock: SingleInstanceLock
    private var initialWindowSize: CGSize {
        guard Bundle.main.object(forInfoDictionaryKey: "SZUOfflinePreview") as? Bool == true else {
            return CGSize(width: 920, height: 740)
        }
        return CGSize(width: max(DashboardLayout.minimumWindowWidth, (Bundle.main.object(forInfoDictionaryKey: "SZUPreviewWidth") as? NSNumber)?.doubleValue ?? 920),
                      height: max(520, (Bundle.main.object(forInfoDictionaryKey: "SZUPreviewHeight") as? NSNumber)?.doubleValue ?? 740))
    }
    #endif

    init() {
        #if os(macOS)
        AppStartupSignals.configure()
        instanceLock = Self.acquireInstanceOrExit()
        #endif
        _model = StateObject(wrappedValue: AppModel())
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
            startupFailure("深大随连已在运行或启动中，请稍后从菜单栏打开已有窗口。")
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

    private static func startupFailure(_ message: String = "暂时无法启动。请检查本机缓存目录的访问权限，或重启 Mac 后重试。") -> Never {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "深大随连暂时无法启动"
        alert.informativeText = message
        alert.addButton(withTitle: "退出")
        alert.runModal()
        exit(1)
    }
    #endif

    var body: some Scene {
        #if os(macOS)
        // A Window scene is a singleton. Repeated menu-bar opens must reuse it.
        Window("深大随连", id: "dashboard") {
            ContentView().environmentObject(model)
                .onChange(of: phase, initial: true) { _, value in model.setForeground(value == .active) }
        }
        .defaultSize(width: initialWindowSize.width, height: initialWindowSize.height)
        .windowResizability(.contentMinSize)
        .windowToolbarStyle(.unified)
        .commands {
            CommandGroup(replacing: .newItem) {}
            DashboardCommands()
        }

        // Window style shows the same status, actions and guard mode as the main window.
        // Its "打开主窗口" reuses the single dashboard through DashboardApplicationDelegate.
        MenuBarExtra {
            MenuBarPanel().environmentObject(model)
        } label: {
            // Template images (Assets.xcassets/MenuBar-*) follow the menu bar's light/dark rendering.
            Image(model.menuBarImageName).accessibilityLabel(SharedCopy.t("app.name"))
        }
        .menuBarExtraStyle(.window)
        #else
        WindowGroup("深大随连", id: "dashboard") {
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

    static func showExistingWindow() -> Bool {
        guard let window = NSApp.windows.first(where: { $0.identifier == windowIdentifier }) else { return false }
        if window.isMiniaturized { window.deminiaturize(nil) }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        return true
    }

    static func showDashboard(using openWindow: OpenWindowAction) {
        if !showExistingWindow() { openWindow(id: "dashboard") }
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        // Restore minimized and previously closed windows before requesting a new scene.
        !Self.showExistingWindow()
    }
}

/// Identifies only the dashboard; menu-bar windows and export panels are excluded.
struct DashboardWindowRegistration: NSViewRepresentable {
    let toolbarContent: AnyView

    func makeCoordinator() -> Coordinator { Coordinator(content: toolbarContent) }

    func makeNSView(context: Context) -> NSView {
        let view = RegisteredDashboardView()
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

    private final class RegisteredDashboardView: NSView {
        weak var coordinator: Coordinator?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window?.identifier = DashboardApplicationDelegate.windowIdentifier
            window?.tabbingMode = .disallowed
            window?.titleVisibility = .hidden
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
            let container = NSView(frame: NSRect(x: 0, y: 0, width: 378, height: 36))
            container.translatesAutoresizingMaskIntoConstraints = false
            container.addSubview(hostingView)
            NSLayoutConstraint.activate([
                container.widthAnchor.constraint(greaterThanOrEqualToConstant: 378),
                // AppKit chooses the width within this range from the available toolbar space.
                container.widthAnchor.constraint(lessThanOrEqualToConstant: 100_000),
                container.heightAnchor.constraint(equalToConstant: 36),
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
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(before: .windowArrangement) {
            Button(SharedCopy.t("action.showWindow")) {
                DashboardApplicationDelegate.showDashboard(using: openWindow)
            }
            .keyboardShortcut("0", modifiers: .command)
        }
    }
}
#endif
