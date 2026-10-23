#if os(macOS)
import SwiftUI

struct WiFiRecoverySettingsView: View {
    @ObservedObject var controller: WiFiRecoveryController
    let rememberedNetworkCount: Int
    let rememberedWiredNetworkCount: Int
    var wiredOnly = false
    var readOnly = false
    var currentNetworkID: String?
    var revealTargets: () -> Void = {}

    private var sameNameNoticeIDs: Set<String> {
        let groups = Dictionary(grouping: controller.allowedNetworks, by: \.displayName)
        var seen = Set<String>()
        return Set(controller.allowedNetworks.compactMap { network in
            guard (groups[network.displayName]?.count ?? 0) > 1, seen.insert(network.displayName).inserted else { return nil }
            return network.id
        })
    }

    private func networkDetail(_ network: WiFiRecoveryNetwork) -> String {
        var parts = [String]()
        if currentNetworkID == network.id { parts.append(SharedCopy.t("memory.currentConnection")) }
        if network.displayName != network.ssid { parts.append(network.ssid) }
        return parts.joined(separator: " · ")
    }

    private var wiredWithoutWiFiMemory: Bool { wiredOnly && controller.allowedNetworks.isEmpty }

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            SectionTitle(text: SharedCopy.t("wifiRecovery.title"))
            Card(padding: SharedTokens.spaceL) {
                VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                    Text(SharedCopy.t("setup.wifiOrder"))
                        .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                    SettingToggleRow(title: SharedCopy.t("wifiRecovery.autoJoin"), detail: SharedCopy.t("wifiRecovery.autoJoinDetail"),
                        isOn: Binding(get: { controller.autoJoinEnabled }, set: { controller.setAutoJoinEnabled($0) }))
                        .disabled(controller.preferencesUnreadable)
                    if !readOnly, controller.autoJoinEnabled, let issue = controller.setupIssue,
                       issue != .noKnownNetworks, issue != .unreadable,
                       !(wiredWithoutWiFiMemory && issue == .permissionRequired) {
                        Banner(tone: controller.hasExistingConnection ? "info" : "warning",
                            title: SharedCopy.t(controller.hasExistingConnection ? "wifiRecovery.optionalSetupTitle" : "wifiRecovery.setupTitle"),
                            detail: setupMessage(issue))
                        if issue == .permissionRequired { permissionControls }
                    }
                    if !readOnly, controller.autoJoinEnabled, !controller.preferencesUnreadable,
                       !controller.allowedNetworks.isEmpty, controller.selectedNetworkIDs.isEmpty {
                        ActionButton(title: SharedCopy.t("wifiRecovery.selectionRequired.action"), kind: .plain,
                                     action: revealTargets)
                            .accessibilityIdentifier("wifiRecovery.chooseTargets")
                    }
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        SettingToggleRow(title: SharedCopy.t("wifiRecovery.powerOn"), detail: SharedCopy.t("wifiRecovery.powerDetail"),
                            isOn: Binding(get: { controller.powerOnWhenDisconnected }, set: { controller.setPowerOnWhenDisconnected($0) }))
                            .disabled(controller.preferencesUnreadable)
                        if !readOnly && controller.powerOnWhenDisconnected && !controller.autoJoinEnabled {
                            Text(SharedCopy.t("setup.powerNeedsJoin"))
                                .font(AppFont.callout).foregroundStyle(Color.tone("warning"))
                                .fixedSize(horizontal: false, vertical: true)
                                .padding(.leading, 20)
                        }
                    }
                    Divider()
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t("wifiRecovery.networksTitle")).font(AppFont.callout.weight(.medium))
                        if controller.allowedNetworks.isEmpty {
                            Text(SharedCopy.t(rememberedWiredNetworkCount > 0 ? "wifiRecovery.noNetworksWired"
                                : rememberedNetworkCount == 0 ? "wifiRecovery.noRememberedNetworks" : "wifiRecovery.noNetworks"))
                                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                                .padding(.top, SharedTokens.spaceXs)
                        } else {
                            Text(SharedCopy.t("wifiRecovery.targetsDetail"))
                                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                            VStack(alignment: .leading, spacing: 0) {
                                let sameNameNotices = sameNameNoticeIDs
                                ForEach(controller.allowedNetworks) { network in
                                    let detail = networkDetail(network)
                                    if sameNameNotices.contains(network.id) {
                                        Text(SharedCopy.t("memory.sameNameDetail"))
                                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                            .fixedSize(horizontal: false, vertical: true)
                                            .padding(.vertical, SharedTokens.spaceS)
                                            .accessibilityIdentifier("wifi.sameNameDetail." + network.id)
                                    }
                                    Toggle(isOn: Binding(get: { controller.selectedNetworkIDs.contains(network.id) },
                                                        set: { controller.setNetworkSelected(network.id, selected: $0) })) {
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(network.displayName).font(AppFont.callout)
                                                .fixedSize(horizontal: false, vertical: true)
                                            if !detail.isEmpty {
                                                Text(detail).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                                    .fixedSize(horizontal: false, vertical: true)
                                            }
                                        }
                                    }
                                    .frame(minHeight: 32)
                                    .disabled(controller.preferencesUnreadable)
                                    .accessibilityIdentifier("wifi.network." + network.id)
                                }
                            }
                            .padding(.top, SharedTokens.spaceXs)
                        }
                        Text(SharedCopy.t("wifiRecovery.openOnly"))
                            .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                            .padding(.top, SharedTokens.spaceXs)
                    }
                    .id("wifiRecovery.targets")
                    if !readOnly && !controller.autoJoinEnabled && controller.permissionStatus != .authorized && !wiredWithoutWiFiMemory {
                        permissionControls
                    }
                    if !readOnly && controller.status != .noKnownNetworks {
                        HStack(alignment: .top, spacing: SharedTokens.spaceS) {
                            AppIcon(name: "info", size: 14)
                            Text(statusText).font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                        }
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
            .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
    }

    private var statusText: String {
        switch controller.status {
        case .disabled: return SharedCopy.t("wifiRecovery.status.disabled")
        case .paused: return SharedCopy.t("wifiRecovery.status.paused")
        case .connected: return SharedCopy.t("wifiRecovery.status.connected")
        case .noKnownNetworks: return SharedCopy.t("wifiRecovery.status.noKnownNetworks")
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
