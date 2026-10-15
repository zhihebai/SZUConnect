#if os(macOS)
import SwiftUI

struct WiFiRecoverySettingsView: View {
    @ObservedObject var controller: WiFiRecoveryController
    let rememberedNetworkCount: Int

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            SectionTitle(text: SharedCopy.t("wifiRecovery.title"))
            Card(padding: SharedTokens.spaceL) {
                VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                    Text(SharedCopy.t("setup.wifiOrder"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Toggle(SharedCopy.t("wifiRecovery.autoJoin"), isOn: Binding(
                            get: { controller.autoJoinEnabled }, set: { controller.setAutoJoinEnabled($0) }))
                            .font(AppFont.callout).frame(minHeight: 32)
                            .disabled(controller.preferencesUnreadable)
                        Text(SharedCopy.t("wifiRecovery.autoJoinDetail"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(.leading, 20)
                    }
                    if controller.autoJoinEnabled, let issue = controller.setupIssue {
                        Banner(tone: "warning", title: SharedCopy.t("wifiRecovery.setupTitle"), detail: setupMessage(issue))
                        if issue == .permissionRequired { permissionControls }
                    }
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Toggle(SharedCopy.t("wifiRecovery.powerOn"), isOn: Binding(
                            get: { controller.powerOnWhenDisconnected }, set: { controller.setPowerOnWhenDisconnected($0) }))
                            .font(AppFont.callout).frame(minHeight: 32)
                            .disabled(controller.preferencesUnreadable)
                        Text(SharedCopy.t("wifiRecovery.powerDetail"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(.leading, 20)
                        if controller.powerOnWhenDisconnected && !controller.autoJoinEnabled {
                            Text(SharedCopy.t("setup.powerNeedsJoin"))
                                .font(AppFont.caption).foregroundStyle(Color.tone("warning"))
                                .fixedSize(horizontal: false, vertical: true)
                                .padding(.leading, 20)
                        }
                    }
                    Divider()
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t("wifiRecovery.networksTitle")).font(AppFont.callout.weight(.medium))
                        Text(SharedCopy.t("wifiRecovery.targetsDetail"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        if controller.allowedNetworks.isEmpty {
                            Text(SharedCopy.t(rememberedNetworkCount == 0 ? "wifiRecovery.noRememberedNetworks" : "wifiRecovery.noNetworks"))
                                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                                .padding(.top, SharedTokens.spaceXs)
                        } else {
                            VStack(alignment: .leading, spacing: 0) {
                                ForEach(controller.allowedNetworks) { network in
                                    Toggle(isOn: Binding(get: { controller.selectedNetworkIDs.contains(network.id) },
                                                        set: { controller.setNetworkSelected(network.id, selected: $0) })) {
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(network.displayName).font(AppFont.callout)
                                            if network.displayName != network.ssid {
                                                Text(network.ssid).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                            }
                                        }
                                    }
                                    .frame(minHeight: 32)
                                    .disabled(controller.preferencesUnreadable)
                                }
                            }
                            .padding(.top, SharedTokens.spaceXs)
                        }
                        Text(SharedCopy.t("wifiRecovery.openOnly"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            .padding(.top, SharedTokens.spaceXs)
                    }
                    if !controller.autoJoinEnabled && controller.permissionStatus != .authorized { permissionControls }
                    HStack(alignment: .top, spacing: SharedTokens.spaceS) {
                        AppIcon(name: "info", size: 14)
                        Text(statusText).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    }
                    if controller.preferencesUnreadable {
                        Banner(tone: "warning", title: SharedCopy.t("wifiRecovery.unreadable"))
                    } else if let error = controller.errorMessage { Banner(tone: "warning", title: error) }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private func setupMessage(_ issue: WiFiRecoverySetupIssue) -> String {
        switch issue {
        case .unreadable: return SharedCopy.t("wifiRecovery.unreadable")
        case .permissionRequired: return SharedCopy.t("wifiRecovery.setupPermission")
        case .noKnownNetworks: return SharedCopy.t("wifiRecovery.setupNoKnown")
        case .noSelection: return SharedCopy.t("wifiRecovery.setupNoSelection")
        }
    }

    @ViewBuilder private var permissionControls: some View {
        ActionButton(title: SharedCopy.t("wifiRecovery.permissionButton")) { controller.requestWiFiNamePermission() }
        Text(SharedCopy.t(controller.permissionStatus == .denied ? "wifiRecovery.permissionDenied" : "wifiRecovery.permissionDetail"))
            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
    }

    private var statusText: String {
        switch controller.status {
        case .disabled: return SharedCopy.t("wifiRecovery.status.disabled")
        case .paused: return SharedCopy.t("wifiRecovery.status.paused")
        case .connected: return SharedCopy.t("wifiRecovery.status.connected")
        case .noSelection: return SharedCopy.t("wifiRecovery.status.noSelection")
        case .searching: return SharedCopy.t("wifiRecovery.status.searching")
        case .waiting: return SharedCopy.t("wifiRecovery.status.waiting")
        case .wifiOff: return SharedCopy.t("wifiRecovery.status.wifiOff")
        case .poweredOn: return SharedCopy.t("wifiRecovery.status.poweredOn")
        case .joined: return SharedCopy.t("wifiRecovery.status.joined")
        case .systemConnectionRequired: return SharedCopy.t("wifiRecovery.status.systemConnectionRequired")
        case .permissionRequired: return SharedCopy.t("wifiRecovery.status.permissionRequired")
        case .unavailable: return SharedCopy.t("wifiRecovery.status.unavailable")
        case .connectionUnclear: return SharedCopy.t("wifiRecovery.status.connectionUnclear")
        case .failed: return SharedCopy.t("wifiRecovery.status.failed")
        }
    }
}
#endif
