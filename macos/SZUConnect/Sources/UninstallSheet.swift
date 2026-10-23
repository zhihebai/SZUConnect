#if os(macOS)
import AppKit
import SwiftUI

extension AppModel: AppUninstallModel {}

/// Cancel, Escape and the close control all dismiss this confirmation without
/// invoking the service. Once confirmed, do not interrupt a partially finished cleanup.
struct AppUninstallSheet: View {
    @ObservedObject var model: AppModel
    let close: () -> Void
    @StateObject private var controller = AppUninstallController(prepareUIForMaintenance: {
        DashboardApplicationDelegate.prepareForAppMaintenance()
        await CompatibilityRunner.stopAllForAppMaintenance()
    }, finish: { DashboardApplicationDelegate.finishAppUninstall() })
    @State private var removeData = false
    @FocusState private var cancelFocused: Bool

    var body: some View {
        AppConfirmationSheet(
            title: SharedCopy.t("uninstall.title"),
            height: confirmationHeight,
            allowsClosing: !controller.running,
            closeIdentifier: "uninstall.close",
            close: cancel
        ) {
            Text(SharedCopy.t("uninstall.detail"))
                .fixedSize(horizontal: false, vertical: true)
            Text(SharedCopy.t("uninstall.mac.location"))
                .font(AppFont.callout)
                .foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("uninstall.location")
            SettingToggleRow(
                title: SharedCopy.t("uninstall.removeData"),
                detail: SharedCopy.t(removeData ? "uninstall.mac.cleanupWarning" : "uninstall.mac.keepDetail"),
                isOn: $removeData
            )
                .toggleStyle(.checkbox)
                .disabled(controller.preparing || controller.running || model.isMaintaining)
                .accessibilityIdentifier("uninstall.removeData")
            if controller.preparing || controller.running {
                ProgressView(SharedCopy.t(controller.preparing ? "uninstall.mac.preparing" : "uninstall.working"))
                    .accessibilityIdentifier("uninstall.progress")
            }
            if let message = controller.errorMessage {
                Banner(tone: "warning", title: SharedCopy.t(controller.report == nil ? "uninstall.mac.unavailableTitle" : "uninstall.failed"), detail: message)
                    .accessibilityIdentifier("uninstall.error")
            }
            if let report = controller.report {
                Text(SharedCopy.t("uninstall.mac.failureDetail")).font(AppFont.callout)
                Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceL, verticalSpacing: SharedTokens.spaceS) {
                    ForEach(AppUninstallStep.allCases, id: \.rawValue) { step in
                        GridRow {
                            Text(SharedCopy.t("uninstall.mac.step." + step.rawValue))
                            Text(stepStatus(report.steps[step] ?? .pending))
                                .foregroundStyle(Color.token("textSecondary"))
                        }
                    }
                }
                .accessibilityIdentifier("uninstall.results")
            }
            if model.isMaintaining && !controller.running {
                Text(SharedCopy.t("uninstall.mac.paused"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
            }
        } actions: {
            Button(SharedCopy.t("action.cancel"), action: cancel)
                .keyboardShortcut(.cancelAction)
                .focused($cancelFocused)
                .disabled(controller.running)
                .accessibilityIdentifier("uninstall.cancel")
            Button(SharedCopy.t(controller.report == nil ? "uninstall.confirm" : "action.retry"), role: .destructive) {
                Task { await controller.confirm(model: model, removeData: removeData) }
            }
            .buttonStyle(.borderedProminent)
            .disabled(controller.preparing || controller.running || !model.canUninstallApplication)
            .accessibilityIdentifier("uninstall.confirm")
        }
        .defaultFocus($cancelFocused, true)
        .onAppear {
            if model.isMaintaining { removeData = model.uninstallRemovesData }
            controller.configure(model: model)
        }
        .onDisappear { controller.cancelPreparation() }
    }

    private func cancel() {
        guard !controller.running else { return }
        controller.cancelPreparation()
        close()
    }

    private var confirmationHeight: CGFloat {
        if controller.report != nil { return 570 }
        return controller.errorMessage == nil ? 340 : 460
    }

    private func stepStatus(_ status: AppUninstallStepState) -> String {
        let key: String
        switch status {
        case .completed: key = "completed"
        case .notRequested: key = "notRequested"
        case .pending: key = "pending"
        case .failed: key = "failed"
        }
        return SharedCopy.t("uninstall.mac.status." + key)
    }
}
#endif
