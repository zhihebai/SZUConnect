#if os(macOS)
import AppKit

/// Complete a successful uninstall after its writers and workers have stopped.
/// AppKit must leave every sheet session before it can terminate an application.
@MainActor final class AppUninstallLifecycle {
    private let application: NSApplication
    private let terminate: () -> Void
    private(set) var isFinishing = false
    private(set) var terminationRequested = false

    init(application: NSApplication? = nil, terminate: (() -> Void)? = nil) {
        let instance = application ?? .shared
        self.application = instance
        self.terminate = terminate ?? { [weak instance] in instance?.terminate(nil) }
    }

    func finish() {
        guard !isFinishing else { return }
        isFinishing = true
        // Let the confirming task clear its busy flag and let SwiftUI finish its
        // current update before ending the nested confirmation/about sheets.
        DispatchQueue.main.async { [self] in dismissWindows() }
    }

    private func dismissWindows() {
        let windows = application.windows
        var visited = Set<ObjectIdentifier>()
        for window in windows { dismiss(window, visited: &visited) }
        // endSheet may complete an AppKit transition on the next run-loop turn.
        // Recheck relationships instead of terminating during that transition.
        DispatchQueue.main.async { [self] in
            guard application.windows.allSatisfy({ $0.attachedSheet == nil && $0.sheetParent == nil }) else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.01) { [self] in dismissWindows() }
                return
            }
            guard !terminationRequested else { return }
            terminationRequested = true
            terminate()
        }
    }

    private func dismiss(_ window: NSWindow, visited: inout Set<ObjectIdentifier>) {
        guard visited.insert(ObjectIdentifier(window)).inserted else { return }
        // A snapshot and child-first recursion work for more than one sheet level;
        // waiting synchronously for attachedSheet to clear would block AppKit.
        if let child = window.attachedSheet { dismiss(child, visited: &visited) }
        if let parent = window.sheetParent { parent.endSheet(window) }
        window.orderOut(nil)
        window.close()
    }
}
#endif
