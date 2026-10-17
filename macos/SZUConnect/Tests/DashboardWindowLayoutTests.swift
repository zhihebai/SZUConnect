import AppKit
import SwiftUI

@MainActor
private final class LayoutPageState: ObservableObject {
    @Published var expanded = false
}

private struct LayoutTestPage: View {
    @ObservedObject var state: LayoutPageState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                ForEach(0..<(state.expanded ? 40 : 2), id: \.self) { index in
                    Text("Test row \(index): " + String(repeating: "wrappable sample text ", count: state.expanded ? 8 : 1))
                }
            }.padding(24).frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(idealWidth: state.expanded ? 1100 : 720, idealHeight: state.expanded ? 1100 : 680)
    }
}

@MainActor
private final class LayoutTestToolbar: NSObject, NSToolbarDelegate {
    let toolbar = NSToolbar(identifier: "layout-test.toolbar")
    private let item = NSToolbarItem(itemIdentifier: NSToolbarItem.Identifier("navigation"))

    override init() {
        let content = HStack(spacing: 8) {
            Text("深大随连").fixedSize()
            Spacer(minLength: 8)
            ForEach(["连接", "记录", "设置"], id: \.self) { title in
                Button(title) {}.frame(minWidth: 74, minHeight: 32)
            }
            Spacer(minLength: 8)
            Button("?") {}.frame(width: 36, height: 36)
        }.frame(minWidth: DashboardLayout.toolbarMinimumWidth, maxWidth: .infinity)
            .frame(height: DashboardLayout.toolbarHeight)
        let hostingView = NSHostingView(rootView: content)
        hostingView.sizingOptions = []
        hostingView.translatesAutoresizingMaskIntoConstraints = false
        let container = NSView(frame: NSRect(x: 0, y: 0, width: DashboardLayout.toolbarMinimumWidth,
                                            height: DashboardLayout.toolbarHeight))
        container.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(hostingView)
        NSLayoutConstraint.activate([
            container.widthAnchor.constraint(greaterThanOrEqualToConstant: DashboardLayout.toolbarMinimumWidth),
            container.widthAnchor.constraint(lessThanOrEqualToConstant: 100_000),
            container.heightAnchor.constraint(equalToConstant: DashboardLayout.toolbarHeight),
            hostingView.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            hostingView.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            hostingView.topAnchor.constraint(equalTo: container.topAnchor),
            hostingView.bottomAnchor.constraint(equalTo: container.bottomAnchor)
        ])
        container.setContentHuggingPriority(.defaultLow, for: .horizontal)
        item.view = container
        item.isBordered = false
        item.visibilityPriority = .high
        toolbar.allowsUserCustomization = false
        toolbar.autosavesConfiguration = false
        toolbar.displayMode = .iconOnly
        super.init()
        toolbar.delegate = self
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { [item.itemIdentifier] }
    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { [item.itemIdentifier] }
    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier identifier: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? { item }
}

@main
@MainActor
struct DashboardWindowLayoutTests {
    static func main() {
        let application = NSApplication.shared
        application.setActivationPolicy(.accessory)
        application.finishLaunching()
        var count = 0
        func expect(_ condition: Bool, _ message: String) {
            precondition(condition, message)
            count += 1
            print("PASS: " + message)
            fflush(stdout)
        }
        func settle(_ window: NSWindow) {
            for _ in 0..<4 {
                window.contentView?.layoutSubtreeIfNeeded()
                RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.04))
            }
            expect(!window.isVisible, "Test window remains hidden")
        }
        func evidence(_ label: String, _ window: NSWindow) {
            print("SIZE \(label): frame=\(NSStringFromRect(window.frame)) minimum=\(NSStringFromSize(window.minSize)) toolbarItems=\(window.toolbar?.visibleItems?.count ?? 0)")
            fflush(stdout)
        }
        let state = LayoutPageState()
        let window = NSWindow(contentRect: NSRect(x: 200, y: 200, width: 720, height: 680),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.titleVisibility = .hidden
        window.toolbarStyle = .unified
        window.tabbingMode = .disallowed
        let host = DashboardLayout.makeHostingView(LayoutTestPage(state: state))
        window.contentView = host
        let toolbar = LayoutTestToolbar()
        window.toolbar = toolbar.toolbar
        settle(window)
        evidence("initial", window)
        expect(window.minSize.width >= 500 && window.minSize.height >= 520, "Native minimum survives initial SwiftUI layout")

        // This reproduces a drag/programmatic resize below the limit after initial layout.
        window.setFrame(NSRect(x: 200, y: 200, width: 320, height: 400), display: false)
        settle(window)
        evidence("small-request", window)
        expect(window.frame.width >= 500 && window.frame.height >= 520, "Undersized requests cannot leave a narrow native window")
        expect(window.toolbar?.visibleItems?.count == 1, "The navigation group stays visible at minimum width")

        let usableScreen = window.screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let narrowSavedFrame = NSRect(x: usableScreen.minX + 60, y: usableScreen.maxY - 460, width: 320, height: 400)
        DashboardLayout.restoreFrame(narrowSavedFrame, in: window)
        settle(window)
        evidence("restored-small", window)
        expect(window.frame.width >= 500 && window.frame.height >= 520, "An old narrow saved frame is repaired")
        expect(abs(window.frame.maxY - narrowSavedFrame.maxY) < 1, "Growing a restored frame preserves its top edge")

        window.setFrame(NSRect(x: usableScreen.minX + 60, y: usableScreen.maxY - 760, width: 720, height: 700), display: false)
        settle(window)
        let userFrame = window.frame
        DashboardLayout.restoreFrame(userFrame, in: window)
        settle(window)
        expect(window.frame == userFrame, "Restoring a valid user frame keeps its size and position")
        state.expanded = true
        settle(window)
        evidence("long-page", window)
        expect(window.frame == userFrame, "Changing to long content and a larger ideal size does not resize the window")
        state.expanded = false
        settle(window)
        expect(window.frame == userFrame, "Returning to short content does not resize the window")

        window.contentView = DashboardLayout.makeHostingView(LayoutTestPage(state: state))
        settle(window)
        expect(window.frame == userFrame, "Recreating the content host preserves the user frame")
        window.toolbar = nil
        settle(window)
        window.toolbar = toolbar.toolbar
        settle(window)
        DashboardLayout.restoreFrame(narrowSavedFrame, in: window)
        settle(window)
        evidence("reattached", window)
        expect(window.frame.width >= 500 && window.toolbar?.visibleItems?.count == 1, "Replacing content and toolbar preserves the minimum and navigation")

        guard let fallbackScreen = window.screen ?? NSScreen.main else {
            preconditionFailure("Native window layout tests require an available display")
        }
        let fallbackVisibleFrame = fallbackScreen.visibleFrame
        let removedDisplayX = (NSScreen.screens.map { $0.visibleFrame.maxX }.max() ?? fallbackVisibleFrame.maxX) + 2000
        let removedDisplayFrame = NSRect(x: removedDisplayX, y: fallbackVisibleFrame.maxY - 660, width: 720, height: 600)
        DashboardLayout.restoreFrame(removedDisplayFrame, in: window)
        settle(window)
        evidence("removed-display", window)
        expect(window.frame.minX >= fallbackVisibleFrame.minX && window.frame.maxX <= fallbackVisibleFrame.maxX,
               "A frame from a removed display returns horizontally inside the current display")
        expect(window.frame.maxY <= fallbackVisibleFrame.maxY && window.frame.maxY > fallbackVisibleFrame.minY,
               "A restored off-screen window has an accessible titlebar")
        expect(window.frame.size == removedDisplayFrame.size, "Recovering an off-screen frame preserves its valid size")
        expect(window.toolbar?.visibleItems?.count == 1, "Navigation stays visible after recovering a removed display")

        let oversizedFrame = NSRect(x: removedDisplayX, y: fallbackVisibleFrame.maxY - 660,
                                    width: fallbackVisibleFrame.width + 200, height: 600)
        DashboardLayout.restoreFrame(oversizedFrame, in: window)
        settle(window)
        evidence("oversized-display", window)
        expect(window.frame.minX == fallbackVisibleFrame.minX, "An oversized restored window aligns its left edge with the display")
        expect(window.frame.width == oversizedFrame.width, "An oversized restored window keeps the user's width")
        DashboardLayout.restoreFrame(userFrame, in: window)
        settle(window)
        expect(window.frame == userFrame, "A valid frame remains unchanged after recovering off-screen windows")
        let beforeInvalidFrame = window.frame
        DashboardLayout.restoreFrame(NSRect(x: CGFloat.nan, y: 0, width: 720, height: 680), in: window)
        expect(window.frame == beforeInvalidFrame, "Invalid saved coordinates do not reach AppKit")
        withExtendedLifetime(toolbar) {}
        print("Dashboard window layout: \(count) passed")
    }
}
