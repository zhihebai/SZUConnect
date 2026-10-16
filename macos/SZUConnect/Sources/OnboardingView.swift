import SwiftUI
#if os(macOS)
import AppKit
#endif

/// Setup follows live state and uses the ordinary controls. Wi-Fi association is optional.
struct ConnectionSetupView: View {
    @EnvironmentObject private var model: AppModel
    let open: (AppSheet) -> Void
    var openNetworkSettings: () -> Void = {}
    @State private var expanded = true

    private var hasMemory: Bool { model.networkRemembered || model.area == .teaching }
    private var accountSaved: Bool { model.hasSavedCredentials && !model.hasUnsavedCredentialChanges }
    private var automatic: Bool { model.monitoringMode == .recover }
    private var step: ConnectionSetupStep {
        .next(networkAvailable: model.networkAvailable, hasNetworkMemory: hasMemory,
              accountSaved: accountSaved, automatic: automatic)
    }
    private var complete: Int { [hasMemory, accountSaved, automatic].filter { $0 }.count }

    var body: some View {
        if !model.sessionReadOnly {
            Card(padding: SharedTokens.spaceM) {
                if step == .ready {
                    ViewThatFits(in: .horizontal) {
                        HStack(spacing: 8) { readyContent }
                        VStack(alignment: .leading, spacing: 8) { readyContent }
                    }
                } else {
                    DisclosureGroup(isExpanded: $expanded) {
                        VStack(alignment: .leading, spacing: 8) {
                            Text(SharedCopy.t("setup." + step.rawValue + ".body"))
                                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                .fixedSize(horizontal: false, vertical: true)
                            ViewThatFits(in: .horizontal) {
                                HStack(spacing: 8) { setupActions }
                                VStack(alignment: .leading, spacing: 8) { setupActions }
                            }
                        }.padding(.top, 6)
                    } label: {
                        HStack(spacing: 8) {
                            Text(SharedCopy.t("setup.title")).font(AppFont.callout.weight(.semibold))
                            Spacer(minLength: 0)
                            Text(SharedCopy.f("setup.progress", ["count": String(complete)]))
                                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        }
                    }
                    .disclosureGroupStyle(FullRowDisclosureStyle())
                }
            }
            .accessibilityIdentifier("setup.checklist")
        }
    }

    @ViewBuilder private var readyContent: some View {
        Label {
            Text(SharedCopy.t("setup.ready")).font(AppFont.caption)
        } icon: { AppIcon(name: "check", size: 14, color: .token("success")) }
        #if os(macOS)
        WiFiSetupShortcut(controller: model.wifiRecovery, open: { open(.wifiRecovery) })
        #endif
    }

    @ViewBuilder private var setupActions: some View {
        switch step {
        case .network:
            #if os(macOS)
            ActionButton(title: SharedCopy.t("setup.network.action"), icon: "signal") {
                guard !model.isPreview,
                      let url = URL(string: "x-apple.systempreferences:com.apple.wifi-settings-extension"),
                      NSWorkspace.shared.open(url) else { open(.help); return }
            }
            #else
            EmptyView()
            #endif
        case .memory:
            ActionButton(title: SharedCopy.t("setup.memory.action"), icon: "bookmark", action: openNetworkSettings)
        case .account:
            ActionButton(title: SharedCopy.t("setup.account.action"), icon: "user") { open(.account) }
        case .recovery:
            ActionButton(title: SharedCopy.t("setup.recovery.action"), icon: "shield-check") { model.setMonitoringMode(.recover) }
                .disabled(model.busy)
        case .ready: EmptyView()
        }
        ActionButton(title: SharedCopy.t("setup.steps"), kind: .plain) { open(.help) }
    }
}

struct QuickStartHelpView: View {
    var body: some View {
        Card(padding: SharedTokens.spaceL) {
            SectionTitle(text: SharedCopy.t("setup.helpTitle"))
            Text(SharedCopy.t("setup.helpDetail"))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            ForEach(["setup.helpNetwork", "setup.helpAccount", "setup.helpCheck", "setup.helpMemory", "setup.helpRecovery", "setup.helpWifi"], id: \.self) { key in
                VStack(alignment: .leading, spacing: 4) {
                    Text(SharedCopy.t(key + "Title")).font(AppFont.callout.weight(.medium))
                    Text(SharedCopy.t(key == "setup.helpWifi" ? "setup.wifi.body" : key))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .accessibilityIdentifier("help.quickStart")
    }
}

#if os(macOS)
private struct WiFiSetupShortcut: View {
    @ObservedObject var controller: WiFiRecoveryController
    let open: () -> Void
    var body: some View {
        Button(SharedCopy.t(controller.autoJoinEnabled ? "setup.wifi.manage" : "setup.wifi.optional"), action: open)
            .font(AppFont.caption).buttonStyle(.plain).foregroundStyle(Color.token("accent"))
            .accessibilityIdentifier("setup.wifi")
    }
}
#endif

struct TourAnchorPreference: PreferenceKey {
    static var defaultValue: [TourTarget: Anchor<CGRect>] = [:]
    static func reduce(value: inout [TourTarget: Anchor<CGRect>], nextValue: () -> [TourTarget: Anchor<CGRect>]) {
        value.merge(nextValue(), uniquingKeysWith: { _, latest in latest })
    }
}

extension View {
    func tourTarget(_ target: TourTarget) -> some View {
        anchorPreference(key: TourAnchorPreference.self, value: .bounds) { [target: $0] }
    }
}

/// This layer never intercepts the controls it explains, including scrolling and cancel.
struct TourSpotlight: View {
    let target: Anchor<CGRect>?
    var body: some View {
        GeometryReader { geometry in
            if let target {
                let visible = geometry[target].insetBy(dx: -3, dy: -3)
                    .intersection(CGRect(origin: .zero, size: geometry.size))
                if !visible.isNull && visible.height > 12 {
                    Path { path in
                        path.addRect(CGRect(origin: .zero, size: geometry.size))
                        path.addRoundedRect(in: visible, cornerSize: CGSize(width: 14, height: 14))
                    }
                    .fill(Color.black.opacity(0.28), style: FillStyle(eoFill: true))
                    RoundedRectangle(cornerRadius: 14)
                        .stroke(Color.token("accent"), lineWidth: 2)
                        .frame(width: visible.width, height: visible.height)
                        .position(x: visible.midX, y: visible.midY)
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// An in-window coach, separated from the real page so explanations cannot cover a control.
struct OnboardingView: View {
    @EnvironmentObject private var model: AppModel
    let progress: TourProgress
    let back: () -> Void
    let next: () -> Void
    let refocus: () -> Void
    let close: () -> Void

    private var step: TourStep { progress.step }
    private var freshCheck: Bool { progress.hasFreshCheck(completedAt: model.lastCheckedAt, busy: model.busy) }
    private var needsAction: Bool {
        (step == .detection && !freshCheck) || (step == .account && !model.hasSavedCredentials)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 10) {
                Text(SharedCopy.t("guide." + step.rawValue + ".title"))
                    .font(AppFont.headline).accessibilityAddTraits(.isHeader)
                Spacer(minLength: 0)
                Text("\(progress.index + 1) / \(progress.steps.count)")
                    .font(AppFont.caption).monospacedDigit().foregroundStyle(Color.token("textSecondary"))
                Button(action: close) {
                    AppIcon(name: "x", size: 15).frame(width: 28, height: 28).contentShape(Rectangle())
                }
                .buttonStyle(.plain).keyboardShortcut(.cancelAction)
                .accessibilityLabel(SharedCopy.t("guide.exit"))
                .accessibilityIdentifier("tutorial.close")
            }
            Text(SharedCopy.t("guide." + step.rawValue + ".body"))
                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            if let statusText {
                Text(statusText).font(AppFont.caption)
                    .foregroundStyle(Color.token("accent"))
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.updatesFrequently)
            }
            HStack(spacing: 8) {
                if progress.index > 0 { Button(SharedCopy.t("tutorial.back"), action: back) }
                if step.target != nil {
                    Button(SharedCopy.t("guide.focus"), action: refocus).buttonStyle(.plain)
                        .foregroundStyle(Color.token("textSecondary"))
                }
                Spacer(minLength: 0)
                Button(SharedCopy.t(needsAction ? "guide.skipStep" : progress.isLast ? "tutorial.finish" : step == .welcome ? "guide.start" : "tutorial.next"), action: next)
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("tutorial.next")
            }
            .controlSize(.regular)
        }
        .padding(.horizontal, 20).padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.token("surface"))
        .overlay(alignment: .bottom) { Divider() }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("tutorial.coach")
    }

    private var statusText: String? {
        switch step {
        case .detection:
            if model.busy { return SharedCopy.t("guide.check.running") }
            if freshCheck { return SharedCopy.f("guide.check.done", ["result": SharedCopy.t(model.presentation.headline)]) }
            return SharedCopy.t("guide.check.waiting")
        case .account:
            return SharedCopy.t(model.hasUnsavedCredentialChanges ? "guide.account.unsaved" : model.hasSavedCredentials ? "guide.account.saved" : "guide.account.empty")
        case .login:
            if model.busy { return SharedCopy.t("guide.login.running") }
            return SharedCopy.t(model.isOnline ? "guide.login.online" : "guide.login.waiting")
        case .memory:
            return SharedCopy.t(model.networkRemembered ? "guide.memory.saved" : "guide.memory.waiting")
        case .recovery:
            return SharedCopy.f("guide.recovery.mode", ["mode": model.monitoringMode.title])
        default: return nil
        }
    }
}
