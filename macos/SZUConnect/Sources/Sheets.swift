import SwiftUI

struct NetworkDetailsView: View {
    @EnvironmentObject private var model: AppModel
    let openProxyAssistant: () -> Void
    @State private var proxy = ProxyRules.summarize(ProxyRules.readSystem())

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                HStack(spacing: SharedTokens.spaceXs) {
                    Text(SharedCopy.t("env.connectionAndProxy"))
                        .font(AppFont.callout.weight(.semibold)).accessibilityAddTraits(.isHeader)
                    InfoButton(topic: "proxyModes")
                    Spacer(minLength: 0)
                    Button {
                        proxy = ProxyRules.summarize(ProxyRules.readSystem())
                        model.refreshDisplayedNetwork()
                    } label: {
                        Label {
                            Text(SharedCopy.t("env.refresh"))
                        } icon: {
                            AppIcon(name: "refresh", size: 14)
                        }
                        .frame(minHeight: 24)
                    }
                    .buttonStyle(.bordered)
                    .disabled(model.busy || model.networkInformationRefreshing)
                    .accessibilityIdentifier("network.refresh")
                }
                Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceL, verticalSpacing: SharedTokens.spaceS) {
                    summaryRow("env.layer.access", value: model.connectionKindTitle, id: "access")
                    summaryRow("env.layer.proxy", value: SharedCopy.t("env.proxy." + proxy.state), id: "proxy")
                    summaryRow("env.layer.tunnel", value: SharedCopy.t("env.tunnel." + (model.tunnelActive ? "detected" : "notDetected")), id: "tunnel")
                }
                Divider()
                Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceL, verticalSpacing: SharedTokens.spaceS) {
                    summaryRow("env.connectionDetails.interfaces", value: model.interfaceSummary, id: "interfaces")
                    if let target = model.authenticationTargetName {
                        summaryRow("env.connectionDetails.authenticationTarget", value: target, id: "authenticationTarget")
                    }
                    if let primary = model.systemPhysicalLabel {
                        summaryRow("env.connectionDetails.systemPrimary", value: primary, id: "systemPrimary")
                    }
                    summaryRow("env.connectionDetails.campusService", value: SharedCopy.t("env.connectionDetails." + model.campusVerificationDisplayState),
                        id: "campusService", highlighted: model.campusVerificationDisplayState == "confirmed")
                }
                if let feedback = model.networkInformationFeedback {
                    Text(feedback).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .accessibilityIdentifier("network.refresh.feedback")
                }
            }
            .padding(SharedTokens.spaceM)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium))

            connectivityComparison
            #if os(macOS)
            ActionButton(title: SharedCopy.t("proxyAssistantMac.title"), icon: "settings", action: openProxyAssistant)
            .accessibilityIdentifier("network.proxyAssistant")
            #endif
        }
        .font(AppFont.callout)
        .onChange(of: model.networkRevision) { _, _ in
            proxy = ProxyRules.summarize(ProxyRules.readSystem())
        }
        .task {
            while !Task.isCancelled && !model.isMaintaining {
                let latest = ProxyRules.summarize(ProxyRules.readSystem())
                if latest != proxy { proxy = latest }
                do { try await Task.sleep(for: .seconds(5)) } catch { return }
            }
        }
    }

    private func summaryRow(_ key: String, value: String, id: String, highlighted: Bool = false) -> some View {
        GridRow(alignment: .top) {
            Text(SharedCopy.t(key)).foregroundStyle(Color.token("textSecondary"))
            Text(value).frame(maxWidth: .infinity, alignment: .leading)
                .foregroundStyle(highlighted ? Color.tone("success") : Color.token("textPrimary"))
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("network.environment." + id)
        }
    }

    private var comparisonAction: some View {
        let checking = model.physicalConnectivityChecking || model.systemConnectivityChecking
        return ActionButton(title: SharedCopy.t(checking ? "action.cancel" : "action.check"), icon: checking ? "x" : "refresh") {
            if checking { model.cancelCurrentOperation() } else { model.checkConnection() }
        }
        .disabled(model.busy && !checking)
        .accessibilityIdentifier("network.check")
    }

    private var connectivityComparison: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            comparisonHeading
            HStack(spacing: SharedTokens.spaceS) {
                ProbeModeSelection(
                    selection: Binding(get: { model.probeMode }, set: { model.setProbeMode($0) }),
                    accessibilityIdentifier: "network.probeMode"
                )
                Spacer(minLength: SharedTokens.spaceS)
                comparisonAction
            }
            connectivityTable
            Text(SharedCopy.t("env.pathComparisonNote"))
                .font(AppFont.callout)
                .foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(SharedTokens.spaceM)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium))
    }

    private var comparisonHeading: some View {
        HStack(spacing: SharedTokens.spaceXs) {
            Text(SharedCopy.t("env.pathComparison"))
                .font(AppFont.callout.weight(.semibold))
                .accessibilityAddTraits(.isHeader)
            InfoButton(topic: "networkComparison")
                .accessibilityIdentifier("network.comparison.info")
            Spacer(minLength: 0)
        }
    }

    private var connectivityTable: some View {
        Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceM, verticalSpacing: SharedTokens.spaceM) {
            GridRow(alignment: .top) {
                Color.clear.frame(width: 70, height: 1).accessibilityHidden(true)
                pathHeading("env.systemProbe", checkedAt: model.systemConnectivity == nil ? nil : model.systemConnectivityCheckedAt,
                            running: model.systemConnectivityChecking)
                pathHeading("env.physicalProbe", checkedAt: model.lastInternetCheck == nil ? nil : model.lastCheckedAt,
                            running: model.physicalConnectivityChecking)
            }
            Divider().gridCellUnsizedAxes(.horizontal)
            connectivityRow("records.detail.apple", system: model.systemConnectivity?.appleResult,
                            physical: model.lastInternetCheck?.base.appleResult)
            Divider().gridCellUnsizedAxes(.horizontal)
            connectivityRow("records.detail.microsoft", system: model.systemConnectivity?.microsoftResult,
                            physical: model.lastInternetCheck?.base.microsoftResult)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityIdentifier("network.comparison")
    }

    private func pathHeading(_ key: String, checkedAt: Date?, running: Bool) -> some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
            Text(SharedCopy.t(key)).font(AppFont.callout.weight(.semibold))
            if !running, let checkedAt {
                Text(checkedAt.formatted(date: .omitted, time: .standard))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            }
        }
        .fixedSize(horizontal: false, vertical: true)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func connectivityRow(_ key: String, system: ConnectivityEndpointResult?,
                                 physical: ConnectivityEndpointResult?) -> some View {
        GridRow(alignment: .top) {
            Text(SharedCopy.t(key)).font(AppFont.callout.weight(.medium))
            endpointResult(system, running: model.systemConnectivityChecking)
                .accessibilityLabel(SharedCopy.t(key) + "，" + SharedCopy.t("env.systemProbe") + "，"
                    + endpointText(system, running: model.systemConnectivityChecking))
            endpointResult(physical, running: model.physicalConnectivityChecking)
                .accessibilityLabel(SharedCopy.t(key) + "，" + SharedCopy.t("env.physicalProbe") + "，"
                    + endpointText(physical, running: model.physicalConnectivityChecking))
        }
    }

    private func endpointText(_ result: ConnectivityEndpointResult?, running: Bool) -> String {
        if running { return SharedCopy.t("dashboard.checking") }
        guard let result else { return SharedCopy.t("compat.result.notRun") }
        return result.state == .notCompleted ? SharedCopy.t("dashboard.notWaited") : result.summary
    }

    private func endpointResult(_ result: ConnectivityEndpointResult?, running: Bool) -> some View {
        Text(endpointText(result, running: running))
            .font(AppFont.body)
            .foregroundStyle(result?.succeeded == true && !running ? Color.tone("success") : Color.token("textPrimary"))
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

}

struct HelpSheet: View {
    @ObservedObject var disclosure: HelpDisclosureState
    private let faqs = ["wifi", "cooldown", "noInternet", "rejected", "deviceLimit", "vpn", "nonCampus", "dorm", "cleanup", "official"]
    private let linkGroups: [(title: String, keys: [String])] = [
        ("help.links.account", ["activation", "forgot", "identity", "self"]),
        ("help.links.guides", ["teachingGuide", "library", "dormAccount", "dormFAQ"]),
        ("help.links.support", ["nc", "download", "contact"]),
    ]

    var body: some View {
        SheetPageScroll {
            QuickStartHelpView(expanded: $disclosure.firstUseExpanded)
            Card(padding: 0) {
                VStack(alignment: .leading, spacing: 0) {
                    SheetSectionHeading(icon: "help", title: SharedCopy.t("help.faqTitle"))
                        .padding(.horizontal, SharedTokens.spaceL)
                        .padding(.top, SharedTokens.spaceL)
                        .padding(.bottom, SharedTokens.spaceM)
                    ForEach(faqs, id: \.self) { key in
                        DisclosureGroup(isExpanded: disclosure.faq(key)) {
                            Text(SharedCopy.t("help.faq.\(key).a"))
                                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                                .fixedSize(horizontal: false, vertical: true)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .textSelection(.enabled)
                        } label: {
                            Text(SharedCopy.t("help.faq.\(key).q"))
                                .font(AppFont.callout.weight(.medium))
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .disclosureGroupStyle(SheetFAQStyle())
                        .accessibilityIdentifier("help.faq." + key)
                        if key != faqs.last {
                            Divider().padding(.horizontal, SharedTokens.spaceL)
                        }
                    }
                }.padding(.bottom, SharedTokens.spaceXs)
            }

            Card(padding: SharedTokens.spaceL) {
                SheetSectionHeading(icon: "globe", title: SharedCopy.t("help.linksTitle"))
                ForEach(linkGroups, id: \.title) { group in
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t(group.title))
                            .font(AppFont.caption.weight(.semibold))
                            .foregroundStyle(Color.token("textSecondary"))
                            .accessibilityAddTraits(.isHeader)
                        FlowLayout(spacing: SharedTokens.spaceM) {
                            ForEach(group.keys, id: \.self) { key in
                                if let url = OfficialLinks.url(key) {
                                    SheetExternalLinkRow(title: SharedCopy.t("link.\(key)"), url: url)
                                        .accessibilityIdentifier("help.link." + key)
                                }
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if group.title != linkGroups.last?.title { Divider() }
                }
                Text(SharedCopy.t("link.leaveNote"))
                    .font(AppFont.caption)
                    .foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

struct AboutSheet: View {
    @State private var emailCopied = false
    @State private var emailExampleExpanded = false
    private var version: String {
        SharedCopy.f("about.version", ["version": Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0"])
    }

    var body: some View {
        SheetPageScroll {
            Card(padding: SharedTokens.spaceL) {
                HStack(spacing: SharedTokens.spaceM) {
                    Image("AppIconDisplay").resizable().frame(width: 48, height: 48).accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t("about.productName")).font(AppFont.title)
                        Text(SharedCopy.t("app.tagline"))
                            .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                        Text(version)
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            .accessibilityIdentifier("about.version")
                    }
                    Spacer(minLength: 0)
                }
                Divider()
                Text(SharedCopy.t("app.unofficial"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
            }

            Card(padding: SharedTokens.spaceL) {
                SheetSectionHeading(icon: "globe", title: SharedCopy.t("about.developer.title"))
                Text(SharedCopy.t("about.developer.description"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                if let url = URL(string: SharedLinks.projectURL) {
                    SheetExternalLinkRow(title: SharedLinks.projectURL, url: url)
                        .accessibilityIdentifier("about.project")
                        .help(SharedLinks.projectURL)
                }
                Text(SharedCopy.t("about.project.star"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
            }

            Card(padding: SharedTokens.spaceL) {
                SheetSectionHeading(icon: "user", title: SharedCopy.t("about.contact.title"))
                Text(SharedCopy.t("about.feedback.detail"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                Text(SharedLinks.feedbackEmail).font(AppFont.callout).textSelection(.enabled)
                    .accessibilityIdentifier("about.contact.email")
                Button(SharedCopy.t(emailCopied ? "action.copied" : "about.feedback.copyEmail")) {
                    Clipboard.copy(SharedLinks.feedbackEmail); emailCopied = true
                }.buttonStyle(.bordered).accessibilityIdentifier("about.feedback.copyEmail")
                Text(SharedCopy.t("about.feedback.subject"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                Text(SharedCopy.t("about.feedback.faultHint"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                DisclosureGroup(isExpanded: $emailExampleExpanded) {
                    Text(SharedCopy.t("about.feedback.example"))
                        .font(AppFont.callout).textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                        .accessibilityIdentifier("about.contact.example.body")
                } label: {
                    Text(SharedCopy.t("about.feedback.exampleTitle"))
                        .font(AppFont.callout.weight(.medium))
                        .accessibilityIdentifier("about.feedback.example")
                }.disclosureGroupStyle(FullRowDisclosureStyle())
            }

            Card(padding: SharedTokens.spaceL) { OpenSourceLicensesView() }
            AccountStorageSection()
        }
    }
}

/// Network probe updates must not invalidate the static project and license cards.
private struct AccountStorageSection: View {
    @EnvironmentObject private var model: AppModel
    @State private var confirmingAccountDeletion = false
    #if os(macOS)
    @State private var showingUninstall = false
    #endif

    var body: some View {
        Card(padding: SharedTokens.spaceL) {
            SheetSectionHeading(icon: "lock", title: SharedCopy.t("about.storage.title"))
            Text(SharedCopy.t("about.storage.body"))
                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
            Button(SharedCopy.t("account.delete"), role: .destructive) { confirmingAccountDeletion = true }
                .buttonStyle(.bordered)
                .disabled(model.isMaintaining || model.sessionReadOnly || model.credentialsDeletionInProgress)
                .accessibilityIdentifier("account.delete")
            if model.credentialsDeletionInProgress {
                ProgressView(SharedCopy.t("account.deleting")).font(AppFont.caption)
            }
            if let message = model.credentialsDeletionMessage {
                Text(message).font(AppFont.caption)
                    .foregroundStyle(model.credentialsDeletionFailed ? Color.token("warning") : Color.token("textSecondary"))
                    .accessibilityIdentifier("account.delete.result")
            }
            #if os(macOS)
            Divider()
            VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                Button(SharedCopy.t("uninstall.open"), role: .destructive) { showingUninstall = true }
                    .buttonStyle(.bordered)
                    .disabled(model.credentialsDeletionInProgress)
                    .accessibilityIdentifier("uninstall.open")
                Text(SharedCopy.t("uninstall.hint"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
            }
            #endif
        }
        #if os(macOS)
        .sheet(isPresented: $confirmingAccountDeletion) {
            AccountDeletionSheet(model: model, close: { confirmingAccountDeletion = false })
        }
        .sheet(isPresented: $showingUninstall) {
            AppUninstallSheet(model: model, close: { showingUninstall = false })
        }
        #else
        .alert(SharedCopy.t("account.delete"), isPresented: $confirmingAccountDeletion) {
            Button(SharedCopy.t("account.delete"), role: .destructive) { Task { await model.deleteSavedCredentials() } }
            Button(SharedCopy.t("action.cancel"), role: .cancel) {}
        } message: { Text(SharedCopy.t("account.deleteDetail")) }
        #endif
    }
}

#if os(macOS)
/// Account removal uses the same confirmation surface as uninstall, but never
/// enters app maintenance or removes settings, network memories or records.
struct AccountDeletionSheet: View {
    @ObservedObject var model: AppModel
    let close: () -> Void
    @State private var startedDeletion = false
    @State private var showsFailure = false
    @FocusState private var cancelFocused: Bool

    private var running: Bool { startedDeletion || model.credentialsDeletionInProgress }

    var body: some View {
        AppConfirmationSheet(
            title: SharedCopy.t("account.delete"),
            height: showsFailure ? 340 : 240,
            allowsClosing: !running,
            closeIdentifier: "account.delete.close",
            close: cancel
        ) {
            Text(SharedCopy.t("account.deleteDetail"))
                .fixedSize(horizontal: false, vertical: true)
            if running {
                ProgressView(SharedCopy.t("account.deleting"))
                    .accessibilityIdentifier("account.delete.progress")
            }
            if showsFailure, let message = model.credentialsDeletionMessage {
                Banner(tone: "warning", title: SharedCopy.t("account.deleteFailedTitle"), detail: message)
                    .accessibilityIdentifier("account.delete.error")
            }
        } actions: {
            Button(SharedCopy.t("action.cancel"), action: cancel)
                .keyboardShortcut(.cancelAction)
                .focused($cancelFocused)
                .disabled(running)
                .accessibilityIdentifier("account.delete.cancel")
            Button(SharedCopy.t(showsFailure ? "action.retry" : "account.delete"), role: .destructive, action: confirm)
                .buttonStyle(.borderedProminent)
                .disabled(running || model.sessionReadOnly || model.isMaintaining)
                .accessibilityIdentifier("account.delete.confirm")
        }
        .defaultFocus($cancelFocused, true)
    }

    private func cancel() {
        guard !running else { return }
        close()
    }

    private func confirm() {
        guard !running, !model.sessionReadOnly, !model.isMaintaining else { return }
        // Gate synchronously so rapid clicks cannot schedule duplicate operations.
        startedDeletion = true
        showsFailure = false
        Task { @MainActor in
            await model.deleteSavedCredentials()
            startedDeletion = false
            if model.credentialsDeletionFailed {
                showsFailure = true
                cancelFocused = true
            } else {
                close()
            }
        }
    }
}
#endif

private struct SheetPageScroll<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: SharedTokens.spaceL) { content }
                .frame(maxWidth: 520, alignment: .leading)
                .padding(SharedTokens.spaceXl)
                .frame(maxWidth: .infinity, alignment: .top)
        }
        .background(Color.token("background"))
    }
}

private struct SheetSectionHeading: View {
    let icon: String
    let title: String

    var body: some View {
        HStack(spacing: SharedTokens.spaceS) {
            AppIcon(name: icon, size: 20, color: .token("accent"))
            Text(title).font(AppFont.headline)
        }
        .foregroundStyle(Color.token("textPrimary"))
        .accessibilityAddTraits(.isHeader)
    }
}

private struct SheetFAQStyle: DisclosureGroupStyle {
    func makeBody(configuration: Configuration) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Button { configuration.isExpanded.toggle() } label: {
                HStack(spacing: SharedTokens.spaceM) {
                    configuration.label
                    Spacer(minLength: 0)
                    AppIcon(name: configuration.isExpanded ? "chevron-down" : "chevron-right", size: SharedTokens.sizeSheetAccessoryIcon,
                            color: .token("textSecondary"))
                }
                .multilineTextAlignment(.leading)
                .foregroundStyle(Color.token("textPrimary"))
                .padding(.horizontal, SharedTokens.spaceL)
                .padding(.vertical, SharedTokens.spaceXs)
                .frame(maxWidth: .infinity, minHeight: SharedTokens.sizeMinTarget, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityValue(SharedCopy.t(configuration.isExpanded ? "disclosure.expanded" : "disclosure.collapsed"))
            if configuration.isExpanded {
                configuration.content
                    .padding(.horizontal, SharedTokens.spaceL)
                    .padding(.bottom, SharedTokens.spaceM)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct SheetExternalLinkRow: View {
    let title: String
    let url: URL
    @Environment(\.openURL) private var openURL

    var body: some View {
        Button { openURL(url) } label: {
            HStack(spacing: SharedTokens.spaceXs) {
                Text(title).fixedSize(horizontal: false, vertical: true)
                AppIcon(name: "external-link", size: SharedTokens.sizeSheetAccessoryIcon, color: .token("accent"))
            }
            .font(AppFont.callout.weight(.medium))
            .multilineTextAlignment(.leading)
            .foregroundStyle(Color.token("accent"))
            .padding(.horizontal, SharedTokens.spaceXs)
            .frame(minHeight: SharedTokens.sizeMinTarget, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(.isLink)
    }
}
