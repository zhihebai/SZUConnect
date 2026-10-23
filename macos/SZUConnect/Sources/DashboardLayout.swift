#if os(macOS)
import AppKit
import SwiftUI

enum DashboardLayout {
    static let minimumWindowSize = NSSize(width: 500, height: 520)
    static let defaultWindowSize = NSSize(width: 560, height: 680)
    static let toolbarMinimumWidth: CGFloat = 378
    static let toolbarHeight: CGFloat = 36

    @MainActor
    static func makeHostingView<Content: View>(_ content: Content) -> NSHostingView<some View> {
        let hostingView = NSHostingView(rootView: content.frame(
            minWidth: minimumWindowSize.width, minHeight: minimumWindowSize.height))
        // Export the minimum to AppKit without letting changing ideal content resize the window.
        hostingView.sizingOptions = [.minSize]
        return hostingView
    }

    @MainActor
    static func restoreFrame(_ frame: NSRect, in window: NSWindow) {
        guard frame.origin.x.isFinite, frame.origin.y.isFinite,
              frame.width.isFinite, frame.height.isFinite else { return }
        window.contentView?.layoutSubtreeIfNeeded()
        let convertedMinimum = window.frameRect(forContentRect: NSRect(origin: .zero, size: minimumWindowSize)).size
        // Full-size content includes the titlebar; SwiftUI's published minimum also accounts for its safe area.
        let minimum = NSSize(width: max(convertedMinimum.width, window.minSize.width),
                             height: max(convertedMinimum.height, window.minSize.height))
        let size = NSSize(width: max(frame.width, minimum.width), height: max(frame.height, minimum.height))
        var restored = NSRect(x: frame.minX, y: frame.maxY - size.height, width: size.width, height: size.height)
        func visibleIntersection(_ screen: NSScreen) -> CGFloat {
            let intersection = frame.intersection(screen.visibleFrame)
            return intersection.isNull ? 0 : intersection.width * intersection.height
        }
        let intersectingScreen = NSScreen.screens.max { visibleIntersection($0) < visibleIntersection($1) }
        let targetScreen = intersectingScreen.flatMap { visibleIntersection($0) > 0 ? $0 : nil }
            ?? window.screen ?? NSScreen.main
        if let visibleFrame = targetScreen?.visibleFrame {
            // AppKit constrains the top edge but can leave a restored window beyond a removed display.
            restored.origin.x = restored.width >= visibleFrame.width ? visibleFrame.minX
                : min(max(restored.minX, visibleFrame.minX), visibleFrame.maxX - restored.width)
        }
        var constrained = window.constrainFrameRect(restored, to: targetScreen)
        // Keep the user's width even when AppKit would shrink it to the display width.
        constrained.origin.x = restored.origin.x
        constrained.size.width = restored.width
        window.setFrame(constrained, display: false)
    }
}
#endif
