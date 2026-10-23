#if os(macOS)
import SwiftUI
import AppKit

extension AppModel {
    /// Assets.xcassets/MenuBar-{idle,online,attention,paused,busy} (template images).
    var menuBarImageName: String {
        if busy { return "MenuBar-busy" }
        if monitoringMode == .recover && autoLoginKey == "paused" { return "MenuBar-paused" }
        let tone = presentation.tone
        if isOnline && tone == "success" { return "MenuBar-online" }
        if tone == "warning" || tone == "danger" { return "MenuBar-attention" }
        return "MenuBar-idle"
    }
}

/// Compact companion to the main window: status, next action, pause/resume, open, quit.
struct MenuBarPanel: View {
    @EnvironmentObject private var model: AppModel
    @State private var partialLoginConfirmation: AppModel.ForcedLoginConfirmation?

    var body: some View {
        if model.isMaintaining {
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                Text(SharedCopy.t("uninstall.mac.paused")).font(AppFont.callout)
                Button(SharedCopy.t("tray.quit")) { NSApp.terminate(nil) }
            }
            .padding(SharedTokens.spaceL).frame(width: 300)
        } else {
        let p = model.presentation
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            HStack(spacing: SharedTokens.spaceS) {
                if model.busy { ProgressView().controlSize(.small) } else {
                    AppIcon(name: SharedTokens.tones[p.tone]?.icon ?? "info", size: 22, color: .tone(p.tone))
                }
                Text(SharedCopy.t(p.headline)).font(AppFont.headline)
            }
            Text(SharedCopy.t(p.detail)).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            Chip(text: SharedCopy.t(p.chip), tone: p.chipTone)
            if let error = model.errorMessage { Text(error).font(AppFont.caption).foregroundStyle(Color.tone("warning")) }
            if let error = model.manualNetworkReadError { Text(error).font(AppFont.caption).foregroundStyle(Color.tone("warning")) }
            VStack(spacing: SharedTokens.spaceS) {
                if model.busy {
                    ActionButton(title: SharedCopy.t("action.cancel"), icon: "x", fill: true) { model.cancelCurrentOperation() }
                } else {
                    if model.canOfferPartialLogin {
                        ActionButton(title: SharedCopy.t("action.partialLogin"), icon: "log-in", kind: .primary, fill: true) { model.connect() }
                    }
                    ActionButton(title: SharedCopy.t("tray.check"), icon: "refresh", kind: model.canOfferPartialLogin ? .secondary : .primary, fill: true) { model.checkConnection() }
                    if model.canOfferForcedLogin {
                        ActionButton(title: SharedCopy.t("action.forceLogin"), icon: "log-in", kind: .secondary, fill: true) {
                            partialLoginConfirmation = model.forcedLoginConfirmation()
                        }
                    }
                    if !model.sessionReadOnly && model.monitoringMode != .observe && model.hasCompleteDraft && !model.isOnline {
                        ActionButton(title: SharedCopy.t("tray.login"), icon: "log-in", fill: true) { model.connect() }
                    }
                }
                if model.monitoringMode == .recover && !model.sessionReadOnly && model.hasSavedCredentials {
                    if !model.pausedForAutomatic {
                        ActionButton(title: SharedCopy.t("tray.pause"), icon: "pause", fill: true) { model.pauseAutomaticRecovery() }
                    } else {
                        ActionButton(title: SharedCopy.t("tray.resume"), icon: "play", fill: true) { model.resumeAutomaticRecovery() }
                    }
                }
                ActionButton(title: SharedCopy.t("tray.open"), icon: "home", fill: true) {
                    DashboardApplicationDelegate.showDashboard()
                }
                ActionButton(title: SharedCopy.t("tray.quit"), kind: .plain, fill: true) { NSApp.terminate(nil) }
            }
        }
        .padding(SharedTokens.spaceL)
        .frame(width: 300)
        .background(Color.token("background"))
        .modifier(PartialLoginWarning(confirmation: $partialLoginConfirmation))
        .onDisappear { partialLoginConfirmation = nil }
        }
    }
}
#endif
