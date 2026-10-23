import SwiftUI

struct SettingsPage: View {
    @EnvironmentObject private var model: AppModel
    @Binding private var appearance: String
    let open: (DashboardDestination) -> Void
    @Binding var focusRequest: DashboardFocusRequest?
    @State private var renamingNetwork: MemoryRecord?
    @State private var forgettingNetwork: MemoryRecord?
    @State private var confirmingForgetAll = false
    @State private var networkName = ""
    @FocusState private var forgetCancelFocused: Bool
    @FocusState private var forgetAllCancelFocused: Bool

    private var wiredOnly: Bool {
        model.connectedPhysicalLabels.contains { $0.hasPrefix("有线") }
            && !model.connectedPhysicalLabels.contains { $0.hasPrefix("Wi-Fi") }
    }

    init(open: @escaping (DashboardDestination) -> Void, focusRequest: Binding<DashboardFocusRequest?>, appearance: Binding<String>) {
        self.open = open
        self._focusRequest = focusRequest
        self._appearance = appearance
    }

    var body: some View {
        ScrollViewReader { reader in
            pageContent(revealWiFiTargets: { reader.scrollTo("wifiRecovery.targets", anchor: .top) })
                .onChange(of: focusRequest) { _, request in focus(request, reader: reader) }
                .onAppear { focus(focusRequest, reader: reader) }
        }
        #if os(macOS)
        .sheet(item: $forgettingNetwork) { record in
            AppConfirmationSheet(title: SharedCopy.t("memory.forget"), height: 280,
                                 closeIdentifier: "memory.forget.close", close: { forgettingNetwork = nil }) {
                Text(SharedCopy.f("memory.forgetConfirmDetail", ["name": model.rememberedNetworkDisplayName(for: record.id)]))
                    .fixedSize(horizontal: false, vertical: true)
            } actions: {
                Button(SharedCopy.t("action.cancel")) { forgettingNetwork = nil }
                    .keyboardShortcut(.cancelAction).focused($forgetCancelFocused)
                    .accessibilityIdentifier("memory.forget.cancel")
                Button(SharedCopy.t("memory.forget"), role: .destructive) { forget(record) }
                    .buttonStyle(.borderedProminent).disabled(model.sessionReadOnly || model.isMaintaining)
                    .accessibilityIdentifier("memory.forget.confirm")
            }
            .defaultFocus($forgetCancelFocused, true)
        }
        .sheet(isPresented: $confirmingForgetAll) {
            AppConfirmationSheet(title: SharedCopy.t("memory.forgetAll"), height: 310,
                                 closeIdentifier: "memory.forgetAll.close", close: { confirmingForgetAll = false }) {
                Text(SharedCopy.t("memory.forgetAllConfirmDetail"))
                    .fixedSize(horizontal: false, vertical: true)
            } actions: {
                Button(SharedCopy.t("action.cancel")) { confirmingForgetAll = false }
                    .keyboardShortcut(.cancelAction).focused($forgetAllCancelFocused)
                    .accessibilityIdentifier("memory.forgetAll.cancel")
                Button(SharedCopy.t("memory.forgetAll"), role: .destructive) { forgetAll() }
                    .buttonStyle(.borderedProminent).disabled(model.sessionReadOnly || model.isMaintaining)
                    .accessibilityIdentifier("memory.forgetAll.confirm")
            }.defaultFocus($forgetAllCancelFocused, true)
        }
        #else
        .alert(SharedCopy.t("memory.forgetAll"), isPresented: $confirmingForgetAll) {
            Button(SharedCopy.t("memory.forgetAll"), role: .destructive) { forgetAll() }
            Button(SharedCopy.t("action.cancel"), role: .cancel) { confirmingForgetAll = false }
        } message: { Text(SharedCopy.t("memory.forgetAllConfirmDetail")) }
        .alert(SharedCopy.t("memory.forget"), isPresented: Binding(
            get: { forgettingNetwork != nil },
            set: { if !$0 { forgettingNetwork = nil } }
        ), presenting: forgettingNetwork) { record in
            Button(SharedCopy.t("memory.forget"), role: .destructive) { forget(record) }
            Button(SharedCopy.t("action.cancel"), role: .cancel) { forgettingNetwork = nil }
        } message: { record in
            Text(SharedCopy.f("memory.forgetConfirmDetail", ["name": model.rememberedNetworkDisplayName(for: record.id)]))
        }
        #endif
        .sheet(item: $renamingNetwork) { record in
            #if os(macOS)
            AppConfirmationSheet(title: SharedCopy.t("memory.renameTitle"), height: 380,
                                 closeIdentifier: "memory.rename.close", close: { renamingNetwork = nil }) {
                renameContent(record)
            } actions: { renameActions(record) }
            #else
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                Text(SharedCopy.t("memory.renameTitle")).font(AppFont.headline)
                renameContent(record)
                HStack { Spacer(); renameActions(record) }
            }
            .padding(SharedTokens.spaceL)
            .frame(minWidth: 300, idealWidth: 380)
            #endif
        }
    }

    private func forget(_ record: MemoryRecord) {
        guard !model.sessionReadOnly, !model.isMaintaining else { return }
        model.forgetNetwork(id: record.id)
        forgettingNetwork = nil
    }

    private func forgetAll() {
        guard !model.sessionReadOnly, !model.isMaintaining else { return }
        model.forgetAllNetworks()
        confirmingForgetAll = false
    }

    @ViewBuilder private func renameContent(_ record: MemoryRecord) -> some View {
        Text(SharedCopy.t("memory.renameDetail"))
            .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
            .fixedSize(horizontal: false, vertical: true)
        TextField(SharedCopy.t("memory.namePlaceholder"), text: $networkName)
            .textFieldStyle(.roundedBorder).onSubmit { saveName(record) }
            .accessibilityIdentifier("memory.rename.name")
        Text(SharedCopy.t("memory.namesPrivacy"))
            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            .fixedSize(horizontal: false, vertical: true)
        if let error = model.networkNameError { Banner(tone: "warning", title: error) }
    }

    @ViewBuilder private func renameActions(_ record: MemoryRecord) -> some View {
        Button(SharedCopy.t("memory.nameCancel")) { renamingNetwork = nil }
            .keyboardShortcut(.cancelAction).accessibilityIdentifier("memory.rename.cancel")
        Button(SharedCopy.t("memory.nameSave")) { saveName(record) }
            .keyboardShortcut(.defaultAction).buttonStyle(.borderedProminent)
            .disabled(NetworkLabels.cleanName(networkName).isEmpty || model.sessionReadOnly || model.isMaintaining)
            .accessibilityIdentifier("memory.rename.save")
    }

    private func pageContent(revealWiFiTargets: @escaping () -> Void) -> some View {
        PageScroll(title: SharedCopy.t("settings.title")) {
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                SectionTitle(text: SharedCopy.t("settings.appearance.title"))
                Card(padding: SharedTokens.spaceL) {
                    Picker(SharedCopy.t("settings.appearance.title"), selection: $appearance) {
                        ForEach(AppearancePreference.allCases) { Text($0.title).tag($0.rawValue) }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                }
            }
            #if os(macOS)
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                SectionTitle(text: SharedCopy.t("settings.backgroundTitle"))
                Card(padding: SharedTokens.spaceL) {
                    SettingToggleRow(title: SharedCopy.t("settings.launch.title"), detail: SharedCopy.t("settings.launch.detail"),
                        isOn: Binding(get: { model.launchAtLogin }, set: { model.setLaunchAtLogin($0) }))
                    .disabled(model.sessionReadOnly)
                    .tourTarget(.startup)
                    .id("startup")
                    Divider()
                    SettingToggleRow(title: SharedCopy.t("settings.dock.always"), detail: SharedCopy.t("settings.dock.detail"),
                        isOn: Binding(get: { model.alwaysShowDockIcon }, set: { model.setAlwaysShowDockIcon($0) }))
                        .disabled(model.sessionReadOnly || !model.dockPreferenceAvailable)
                        .help(SharedCopy.t("settings.dock.systemNote"))
                    if let error = model.dockVisibilityError { Banner(tone: "warning", title: error) }
                }
            }
            #endif
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                HStack(spacing: 4) {
                    SectionTitle(text: SharedCopy.t("memory.title"))
                    InfoButton(topic: "memory")
                }
                Text(SharedCopy.t("memory.networkScopeDetail"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("settings.memory.networkScopeDetail")
                if model.memoryUnreadable { Banner(tone: "warning", title: SharedCopy.t("memory.unreadable")) }
                if model.rememberedNetworks.isEmpty {
                    Text(SharedCopy.t("memory.none")).font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                } else {
                    Card(padding: SharedTokens.spaceM) {
                        let sameNameNotices = model.rememberedNetworkSameNameNoticeIDs
                        Text(SharedCopy.f("memory.count", ["count": String(model.rememberedNetworks.count)])).font(AppFont.bodyStrong)
                        ForEach(model.rememberedNetworks) { record in
                            if sameNameNotices.contains(record.id) {
                                Text(SharedCopy.t("memory.sameNameDetail"))
                                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                    .fixedSize(horizontal: false, vertical: true)
                                    .accessibilityIdentifier("memory.sameNameDetail." + record.id)
                            }
                            HStack(spacing: SharedTokens.spaceS) {
                                AppIcon(name: "shield-check", size: 18, color: .token("accent"))
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(model.rememberedNetworkDisplayName(for: record.id))
                                        .font(AppFont.callout.weight(.medium))
                                        .fixedSize(horizontal: false, vertical: true)
                                        .accessibilityIdentifier("memory.name." + record.id)
                                    Text(model.rememberedNetworkDetail(for: record))
                                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                                        .fixedSize(horizontal: false, vertical: true)
                                        .accessibilityIdentifier("memory.detail." + record.id)
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
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .accessibilityElement(children: .contain)
                        }
                    }
                    if model.hasUnnamedRememberedNetworks {
                        Text(SharedCopy.t("memory.legacyName"))
                            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    }
                    ActionButton(title: SharedCopy.t("memory.forgetAll"), kind: .plain) { confirmingForgetAll = true }
                        .disabled(model.sessionReadOnly || model.isMaintaining)
                        .accessibilityIdentifier("memory.forgetAll.open")
                }
                if let error = model.networkNameError { Banner(tone: "warning", title: error) }
                if !model.rememberedNetworks.isEmpty {
                    Text(SharedCopy.t("memory.entry.explanation"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                }
                Text(SharedCopy.t("memory.namesPrivacy")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            }
            #if os(macOS)
            WiFiRecoverySettingsView(controller: model.wifiRecovery, rememberedNetworkCount: model.rememberedNetworks.count,
                rememberedWiredNetworkCount: model.rememberedNetworks.filter { model.networkLabels.entries[$0.id]?.kind == .ethernet }.count,
                wiredOnly: wiredOnly, readOnly: model.sessionReadOnly || model.isPreview,
                currentNetworkID: model.currentRememberedNetworkID, revealTargets: revealWiFiTargets)
                .disabled(model.sessionReadOnly)
                .tourTarget(.wifiRecovery)
                .id("wifiRecovery")
            #endif
            VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                SectionTitle(text: SharedCopy.t("settings.helpTitle"))
                FlowLayout(spacing: SharedTokens.spaceS) { helpButtons }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .tourTarget(.help)
            .id("help")
            if let error = model.errorMessage {
                Banner(tone: "warning", title: error)
            }
            VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                Text(SharedCopy.t("settings.privacy"))
                Text(SharedCopy.t("settings.closeNote"))
            }
            .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
            .fixedSize(horizontal: false, vertical: true)
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
        guard !model.sessionReadOnly, !model.isMaintaining else { return }
        if model.renameRememberedNetwork(id: record.id, name: networkName) { renamingNetwork = nil }
    }

    @ViewBuilder private var helpButtons: some View {
        ActionButton(title: SharedCopy.t("action.rewatchTutorial"), icon: "sparkles") { open(.tutorial) }
        ActionButton(title: SharedCopy.t("action.help"), icon: "help") { open(.help) }
        ActionButton(title: SharedCopy.t("action.about"), icon: "info") { open(.about) }
    }
}
