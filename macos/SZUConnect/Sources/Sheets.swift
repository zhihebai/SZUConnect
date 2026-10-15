import SwiftUI

struct NetworkDetailsView: View {
    @EnvironmentObject private var model: AppModel
    @StateObject private var runner = CompatibilityRunner()
    @State private var proxy = ProxyRules.summarize(ProxyRules.readSystem())
    @State private var copied = false

    @State private var showingDetails = false

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            HStack(alignment: .top, spacing: SharedTokens.spaceL) {
                Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceL, verticalSpacing: SharedTokens.spaceS) {
                    summaryRow("env.layer.access", value: model.connectionKindTitle)
                    summaryRow("env.layer.proxy", value: SharedCopy.t("env.proxy." + proxy.state))
                    summaryRow("env.layer.tunnel", value: SharedCopy.t("env.tunnel." + (model.tunnelActive ? "detected" : "notDetected")))
                }
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
                .accessibilityIdentifier("network.refresh")
            }
            .padding(SharedTokens.spaceM)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium))

            DisclosureGroup(isExpanded: $showingDetails) {
                VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                    Text(SharedCopy.t("env.intro")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                        Text(SharedCopy.t("env.pathComparison")).font(AppFont.callout.weight(.medium))
                        pathResult("env.systemProbe", report: model.systemConnectivity, running: model.systemConnectivityChecking)
                        pathResult("env.physicalProbe", report: model.lastInternetCheck?.base, running: model.isForegroundBusy && !model.isVerifyingMemory)
                        Text(SharedCopy.t("env.systemProbeNote")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        Text(SharedCopy.t("env.campusVerification." + model.campusVerificationState)).font(AppFont.caption)
                        Text(SharedCopy.t("env.historyScope")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    }
                    Divider()
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.f("env.interfaces", ["list": model.interfaceSummary]))
                            .font(AppFont.callout).textSelection(.enabled)
                        Text(SharedCopy.t("env.note.access"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    }
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(proxyText).font(AppFont.callout)
                        Text(SharedCopy.t("env.note.proxy"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        Text(SharedCopy.t("env.note.tunnel"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    }
                    Divider()
                    VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                        Text(SharedCopy.t("internet.strategy")).font(AppFont.callout.weight(.medium))
                        Picker(SharedCopy.t("internet.strategy"), selection: Binding(get: { model.customWebsiteStrategy }, set: { model.setWebsiteStrategy($0) })) {
                            ForEach(WebsiteProbeStrategy.allCases, id: \.self) { Text($0.title).tag($0) }
                        }.pickerStyle(.segmented).labelsHidden().disabled(!model.canEditWebsiteTargets)
                        Text(model.customWebsiteStrategy.sharedDetail).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    }
                    Divider()
                    diagnosis
                    Divider()
                    VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                        Text(SharedCopy.t("compat.rules.title")).font(AppFont.callout.weight(.medium))
                        Text(SharedCopy.t("compat.rules.detail")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        Text(ProxyRules.rulesText).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                        ActionButton(title: SharedCopy.t(copied ? "compat.rules.copied" : "compat.rules.copy"), icon: copied ? "check" : "copy") {
                            Clipboard.copy(ProxyRules.rulesText)
                            copied = true
                        }
                    }
                }
                .fixedSize(horizontal: false, vertical: true)
            } label: {
                Text(SharedCopy.t("env.details")).font(AppFont.callout.weight(.medium))
            }
            .disclosureGroupStyle(FullRowDisclosureStyle())
            .accessibilityIdentifier("network.technicalDisclosure")
        }
        .font(AppFont.callout)
        .onChange(of: showingDetails) { _, expanded in
            if !expanded { runner.reset() }
        }
        .onChange(of: model.networkRevision) { _, _ in
            runner.reset(); proxy = ProxyRules.summarize(ProxyRules.readSystem())
        }
        .task {
            while !Task.isCancelled {
                let latest = ProxyRules.summarize(ProxyRules.readSystem())
                if latest != proxy { runner.reset(); proxy = latest }
                do { try await Task.sleep(for: .seconds(5)) } catch { return }
            }
        }
        .onDisappear { runner.reset() }
    }

    private func summaryRow(_ key: String, value: String) -> some View {
        GridRow(alignment: .top) {
            Text(SharedCopy.t(key)).foregroundStyle(Color.token("textSecondary"))
            Text(value).frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func pathResult(_ key: String, report: ConnectivityReport?, running: Bool) -> some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
            Text(SharedCopy.t(key)).font(AppFont.caption.weight(.semibold))
            if running {
                HStack(spacing: SharedTokens.spaceS) {
                    ProgressView().controlSize(.small)
                    Text(SharedCopy.t("dashboard.checking")).font(AppFont.caption)
                }
            } else {
                Text(report?.summary ?? SharedCopy.t("compat.result.notRun")).font(AppFont.caption)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var diagnosis: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            Text(SharedCopy.t("compat.title")).font(AppFont.callout.weight(.medium))
            Text(SharedCopy.t("compat.intro")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            if runner.running {
                HStack(spacing: SharedTokens.spaceS) {
                    ProgressView()
                    Text(SharedCopy.t("compat.running"))
                }
                ActionButton(title: SharedCopy.t("action.stopDiagnosis"), icon: "x") { runner.cancel() }
            } else {
                ActionButton(title: SharedCopy.t("action.runDiagnosis"), icon: "search", kind: .primary) {
                    runner.run(proxy: proxy.state, tunnel: model.tunnelActive ? "detected" : "notDetected", preview: model.isPreview)
                }
            }
            if let input = runner.input, let verdict = runner.verdict {
                Grid(alignment: .leading, horizontalSpacing: SharedTokens.spaceL, verticalSpacing: SharedTokens.spaceS) {
                    GridRow {
                        Text("")
                        Text(SharedCopy.t("compat.column.direct")).font(AppFont.caption)
                        Text(SharedCopy.t("compat.column.system")).font(AppFont.caption)
                    }
                    GridRow {
                        Text(SharedCopy.t("compat.row.campus"))
                        Text(SharedCopy.t("compat.result." + input.campusDirect))
                        Text(SharedCopy.t("compat.result." + input.campusSystem))
                    }
                    GridRow {
                        Text(SharedCopy.t("compat.row.public"))
                        Text(SharedCopy.t("compat.result." + input.publicDirect))
                        Text(SharedCopy.t("compat.result." + input.publicSystem))
                    }
                }
                Banner(tone: verdict.offerRules ? "warning" : (verdict.verdict == "allReachable" ? "success" : "info"),
                       title: SharedCopy.t("compat.verdict.\(verdict.verdict).title"),
                       detail: SharedCopy.t("compat.verdict.\(verdict.verdict).detail"))
                ForEach(verdict.recommendations, id: \.self) { rec in
                    Text(SharedCopy.t("compat.rec." + rec))
                        .font(AppFont.callout)
                        .foregroundStyle(Color.token("textPrimary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var proxyText: String {
        var text = SharedCopy.t("env.proxy." + proxy.state)
        if !proxy.kinds.isEmpty { text += "（" + proxy.kinds.map { SharedCopy.t("env.proxy.kind." + $0) }.joined(separator: "、") + "）" }
        if proxy.state == "on" { text += " · " + SharedCopy.t("env.bypass." + proxy.campusBypass) }
        return text
    }
}

struct HelpSheet: View {
    private let faqs = ["wifi", "cooldown", "noInternet", "rejected", "vpn", "nonCampus", "dorm", "official"]
    private let linkGroups: [(title: String, keys: [String])] = [
        ("help.links.account", ["activation", "forgot", "identity", "self"]),
        ("help.links.guides", ["teachingGuide", "library", "dormAccount", "dormFAQ"]),
        ("help.links.support", ["nc", "download", "contact"]),
    ]

    var body: some View {
        SheetPageScroll {
            QuickStartHelpView()
            Card(padding: 0) {
                VStack(alignment: .leading, spacing: 0) {
                    SheetSectionHeading(icon: "help", title: SharedCopy.t("help.faqTitle"))
                        .padding(.horizontal, SharedTokens.spaceL)
                        .padding(.top, SharedTokens.spaceL)
                        .padding(.bottom, SharedTokens.spaceM)
                    ForEach(faqs, id: \.self) { key in
                        DisclosureGroup {
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
    private var version: String {
        let info = Bundle.main.infoDictionary
        return SharedCopy.f("about.version", [
            "version": info?["CFBundleShortVersionString"] as? String ?? "0.10.1",
            "build": info?["CFBundleVersion"] as? String ?? "32",
        ])
    }

    var body: some View {
        SheetPageScroll {
            Card(padding: SharedTokens.spaceL) {
                HStack(spacing: SharedTokens.spaceM) {
                    Image("AppIconDisplay").resizable().frame(width: 48, height: 48).accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t("app.name")).font(AppFont.title)
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
                SheetSectionHeading(icon: "book", title: SharedCopy.t("about.creditsTitle"))
                Text(SharedCopy.t("about.creditsBody"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                ForEach(OfficialLinks.credits, id: \.key) { credit in
                    Divider()
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        if let url = URL(string: credit.url) {
                            SheetExternalLinkRow(title: SharedCopy.t("about.\(credit.key).name"), url: url)
                                .accessibilityIdentifier("about.credit." + credit.key)
                        }
                        Text(SharedCopy.t("about.\(credit.key).detail"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            .fixedSize(horizontal: false, vertical: true)
                            .textSelection(.enabled)
                    }
                }
                Divider()
                Text(SharedCopy.t("about.notices"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }

            Card(padding: SharedTokens.spaceL) {
                SheetSectionHeading(icon: "lock", title: SharedCopy.t("about.storage.title"))
                Text(SharedCopy.t("about.storage.body"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }
        }
    }
}

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
