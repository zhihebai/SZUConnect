import Foundation
import SwiftUI

/// A view-only projection of the four completed checks. A webpage response is
/// never promoted to a campus identity or an authenticated session.
struct ProxyAssistantCheckPresentation {
    enum Result: String, CaseIterable {
        case reachable, tls, dns, timeout, network, cancelled, notRun, redirect, authentication, httpStatus, securityPolicy
        var textKey: String { "compat.result." + rawValue }
    }
    struct Row: Identifiable {
        let id: String
        let direct: Result
        let system: Result
        var titleKey: String { "compat.row." + id }
    }
    let rows: [Row]
    var hasSingleTimeout: Bool { rows.flatMap { [$0.direct, $0.system] }.filter { $0 == .timeout }.count == 1 }
    init(campusDirect: String, campusSystem: String, publicDirect: String, publicSystem: String) {
        rows = [Row(id: "campus", direct: Result(rawValue: campusDirect) ?? .notRun,
                    system: Result(rawValue: campusSystem) ?? .notRun),
                Row(id: "public", direct: Result(rawValue: publicDirect) ?? .notRun,
                    system: Result(rawValue: publicSystem) ?? .notRun)]
    }
}

/// The table is shared with native fitting tests. Row titles retain their
/// intrinsic width while result cells wrap inside the remaining space.
struct ProxyAssistantCheckTable: View {
    let results: ProxyAssistantCheckPresentation
    var body: some View {
        Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceS, verticalSpacing: SharedTokens.spaceS) {
            GridRow(alignment: .top) {
                Color.clear.frame(width: 1, height: 1).accessibilityHidden(true)
                resultText(SharedCopy.t("compat.column.direct"), heading: true)
                resultText(SharedCopy.t("compat.column.system"), heading: true)
            }
            ForEach(results.rows) { row in
                GridRow(alignment: .top) {
                    Self.rowTitle(row.titleKey)
                    resultText(SharedCopy.t(row.direct.textKey))
                    resultText(SharedCopy.t(row.system.textKey))
                }
            }
        }
        .padding(SharedTokens.spaceM)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium))
        .accessibilityIdentifier("proxyAssistant.test.results")
    }
    static func rowTitle(_ key: String) -> some View {
        Text(SharedCopy.t(key)).font(AppFont.callout.weight(.medium))
            .fixedSize(horizontal: true, vertical: false)
    }
    private func resultText(_ text: String, heading: Bool = false) -> some View {
        Text(text).font(heading ? AppFont.callout.weight(.semibold) : AppFont.callout)
            .frame(minWidth: 0, maxWidth: .infinity, alignment: .leading)
            .fixedSize(horizontal: false, vertical: true)
    }
}

enum ProxyAssistantLayout {
    /// Small sections fit their contents; long instructions scroll beneath a fixed header.
    static func sheetHeight(content: CGFloat, header: CGFloat, screen: CGFloat) -> CGFloat {
        let available = screen.isFinite && screen > 80 ? screen - 80 : 680
        let maximum = max(180, min(680, available))
        guard content.isFinite, content > 0, header.isFinite, header > 0 else { return min(440, maximum) }
        return min(maximum, max(260, ceil(content + header + 1)))
    }
}

/// One focusable target includes the selected client and the change action.
/// Intrinsic text measurement chooses a separate action line before either label is compressed.
struct ProxyAssistantSelectedClientRow: View {
    let title: String
    let change: () -> Void
    var body: some View {
        Button(action: change) {
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .center, spacing: SharedTokens.spaceM) {
                    titleText.fixedSize(horizontal: true, vertical: false)
                    Spacer(minLength: 0)
                    changeText.fixedSize(horizontal: true, vertical: false)
                }
                VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                    titleText.fixedSize(horizontal: false, vertical: true)
                    changeText.frame(maxWidth: .infinity, alignment: .trailing)
                }
            }
            .frame(maxWidth: .infinity, minHeight: SharedTokens.sizeMinTarget, alignment: .leading)
            .padding(.vertical, SharedTokens.spaceXs)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title + "，" + SharedCopy.t("proxyAssistant.changeClient"))
        .accessibilityIdentifier("proxyAssistant.changeClient")
    }
    private var titleText: some View {
        Text(title).font(AppFont.bodyStrong).foregroundStyle(Color.token("textPrimary"))
            .accessibilityIdentifier("proxyAssistant.selectedClient")
    }
    private var changeText: some View {
        HStack(alignment: .center, spacing: SharedTokens.spaceS) {
            Text(SharedCopy.t("proxyAssistant.changeClient")).font(AppFont.bodyStrong)
            AppIcon(name: "chevron-down", size: 16, color: .token("accent"))
        }.foregroundStyle(Color.token("accent"))
    }
}

/// Copy text and its icon always move as one control; narrow rows stack the
/// entire address and button rather than wrapping one character or icon alone.
struct ProxyAssistantTargetRow: View {
    let text: String
    let buttonTitle: String
    let copied: Bool
    let identifier: String
    let copy: () -> Void
    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(alignment: .center, spacing: SharedTokens.spaceS) {
                target.fixedSize(horizontal: true, vertical: false)
                Spacer(minLength: 0)
                copyButton.fixedSize(horizontal: true, vertical: false)
            }
            VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                target.fixedSize(horizontal: false, vertical: true)
                copyButton.fixedSize(horizontal: true, vertical: false)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    private var target: some View { Text(text).font(AppFont.callout).textSelection(.enabled) }
    private var copyButton: some View {
        Button(action: copy) {
            HStack(alignment: .center, spacing: SharedTokens.spaceXs) {
                AppIcon(name: copied ? "check" : "copy", size: 14)
                Text(buttonTitle).font(AppFont.callout.weight(.medium))
            }
            .frame(minHeight: 32)
        }
        .buttonStyle(.bordered).accessibilityIdentifier(identifier)
    }
}
