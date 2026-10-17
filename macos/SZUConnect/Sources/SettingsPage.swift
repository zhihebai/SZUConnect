import SwiftUI

struct SettingsPage: View {
    @EnvironmentObject private var model: AppModel
    @Binding private var appearance: String
    let open: (AppSheet) -> Void
    @Binding var focusRequest: DashboardFocusRequest?
    @State private var renamingNetwork: MemoryRecord?
    @State private var forgettingNetwork: MemoryRecord?
    @State private var networkName = ""

    init(open: @escaping (AppSheet) -> Void, focusRequest: Binding<DashboardFocusRequest?>, appearance: Binding<String>) {
        self.open = open
        self._focusRequest = focusRequest
        self._appearance = appearance
    }

    var body: some View {
        ScrollViewReader { reader in
            pageContent
                .onChange(of: focusRequest) { _, request in focus(request, reader: reader) }
                .onAppear { focus(focusRequest, reader: reader) }
        }
        .alert(SharedCopy.t("memory.forget"), isPresented: Binding(
            get: { forgettingNetwork != nil },
            set: { if !$0 { forgettingNetwork = nil } }
        ), presenting: forgettingNetwork) { record in
            Button(SharedCopy.t("memory.forget"), role: .destructive) {
                model.forgetNetwork(id: record.id)
                forgettingNetwork = nil
            }
            Button(SharedCopy.t("action.cancel"), role: .cancel) { forgettingNetwork = nil }
        } message: { record in
            Text(SharedCopy.f("memory.forgetConfirmDetail", ["name": model.rememberedNetworkDisplayName(for: record.id)]))
        }
        .sheet(item: $renamingNetwork) { record in
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                Text(SharedCopy.t("memory.renameTitle")).font(AppFont.headline)
                Text(SharedCopy.t("memory.renameDetail"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                TextField(SharedCopy.t("memory.namePlaceholder"), text: $networkName)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { saveName(record) }
                Text(SharedCopy.t("memory.namesPrivacy"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                if let error = model.networkNameError { Banner(tone: "warning", title: error) }
                HStack {
                    Spacer()
                    Button(SharedCopy.t("memory.nameCancel")) { renamingNetwork = nil }.keyboardShortcut(.cancelAction)
                    Button(SharedCopy.t("memory.nameSave")) { saveName(record) }
                        .keyboardShortcut(.defaultAction)
                        .disabled(NetworkLabels.cleanName(networkName).isEmpty)
                }
            }
            .padding(SharedTokens.spaceL)
            .frame(minWidth: 300, idealWidth: 380)
        }
    }

    private var pageContent: some View {
        PageScroll(title: SharedCopy.t("settings.title")) {
            SectionTitle(text: SharedCopy.t("settings.appearance.title"))
            Picker(SharedCopy.t("settings.appearance.title"), selection: $appearance) {
                ForEach(AppearancePreference.allCases) { Text($0.title).tag($0.rawValue) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            #if os(macOS)
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                SectionTitle(text: SharedCopy.t("settings.launch.title"))
                Toggle(isOn: Binding(get: { model.launchAtLogin }, set: { model.setLaunchAtLogin($0) })) {
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t("settings.launch.title")).font(AppFont.body)
                        Text(SharedCopy.t("settings.launch.detail"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .frame(minHeight: SharedTokens.sizeMinTarget)
                .disabled(model.sessionReadOnly)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .tourTarget(.startup)
            .id("startup")
            #endif
            HStack(spacing: 4) {
                SectionTitle(text: SharedCopy.t("memory.title"))
                InfoButton(topic: "memory")
            }
            if model.memoryUnreadable { Banner(tone: "warning", title: SharedCopy.t("memory.unreadable")) }
            if model.rememberedNetworks.isEmpty {
                Text(SharedCopy.t("memory.none")).font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
            } else {
                Card(padding: SharedTokens.spaceM) {
                    Text(SharedCopy.f("memory.count", ["count": String(model.rememberedNetworks.count)])).font(AppFont.bodyStrong)
                    ForEach(model.rememberedNetworks) { record in
                        HStack(spacing: SharedTokens.spaceS) {
                            AppIcon(name: "shield-check", size: 18, color: .token("accent"))
                            VStack(alignment: .leading, spacing: 2) {
                                Text(model.rememberedNetworkDisplayName(for: record.id))
                                    .font(AppFont.callout.weight(.medium))
                                    .fixedSize(horizontal: false, vertical: true)
                                Text(model.rememberedNetworkDetail(for: record))
                                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                Text(SharedCopy.f("memory.verified", ["date": Date(timeIntervalSince1970: record.verified).formatted(date: .abbreviated, time: .shortened)]))
                                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            }
                            Spacer(minLength: 0)
                            Menu {
                                Button(SharedCopy.t("memory.rename")) {
                                    networkName = model.networkLabels.entries[record.id]?.displayName ?? ""
                                    renamingNetwork = record
                                }
                                .disabled(model.networkLabelsUnavailable)
                                Button(SharedCopy.t("memory.forget"), role: .destructive) { forgettingNetwork = record }
                            } label: {
                                Text(SharedCopy.t("memory.manage"))
                                    .font(AppFont.callout)
                                    .frame(minHeight: 32)
                                    .contentShape(Rectangle())
                            }
                            #if os(macOS)
                            .menuStyle(.borderlessButton)
                            #endif
                            .fixedSize()
                            .disabled(model.sessionReadOnly)
                            .accessibilityLabel(SharedCopy.t("memory.manage") + "：" + model.rememberedNetworkDisplayName(for: record.id))
                            .accessibilityIdentifier("memory.manage." + record.id)
                        }
                        .accessibilityElement(children: .contain)
                    }
                }
                if model.hasUnnamedRememberedNetworks {
                    Text(SharedCopy.t("memory.legacyName"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                }
                ActionButton(title: SharedCopy.t("memory.forgetAll"), kind: .plain) { model.forgetAllNetworks() }
                    .disabled(model.sessionReadOnly)
            }
            if let error = model.networkNameError { Banner(tone: "warning", title: error) }
            Text(SharedCopy.t("memory.namesPrivacy")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            #if os(macOS)
            WiFiRecoverySettingsView(controller: model.wifiRecovery, rememberedNetworkCount: model.rememberedNetworks.count)
                .disabled(model.sessionReadOnly)
                .tourTarget(.wifiRecovery)
                .id("wifiRecovery")
            #endif
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                SectionTitle(text: SharedCopy.t("settings.helpTitle"))
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: SharedTokens.spaceS) { helpButtons }
                    VStack(alignment: .leading, spacing: SharedTokens.spaceS) { helpButtons }
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .tourTarget(.help)
            .id("help")
            if let error = model.errorMessage {
                Banner(tone: "warning", title: error)
            }
            Text(SharedCopy.t("settings.privacy")).font(AppFont.caption).foregroundStyle(Color.token("textTertiary"))
            Text(SharedCopy.t("settings.closeNote")).font(AppFont.caption).foregroundStyle(Color.token("textTertiary"))
            if model.sessionReadOnly {
                Text(SharedCopy.t("settings.readOnlyLaunch")).font(AppFont.caption).foregroundStyle(Color.tone("warning"))
            }
        }
    }

    private func focus(_ request: DashboardFocusRequest?, reader: ScrollViewProxy) {
        guard let request else { return }
        Task { @MainActor in
            await Task.yield()
            reader.scrollTo(request.section, anchor: .top)
            focusRequest = nil
        }
    }

    private func saveName(_ record: MemoryRecord) {
        if model.renameRememberedNetwork(id: record.id, name: networkName) { renamingNetwork = nil }
    }

    @ViewBuilder private var helpButtons: some View {
        ActionButton(title: SharedCopy.t("action.rewatchTutorial"), icon: "sparkles") { open(.tutorial) }
        ActionButton(title: SharedCopy.t("action.help"), icon: "help") { open(.help) }
        ActionButton(title: SharedCopy.t("action.about"), icon: "info") { open(.about) }
    }
}
