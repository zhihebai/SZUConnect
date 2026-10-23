#if os(macOS)
import AppKit
import Observation
import SwiftUI
import UniformTypeIdentifiers

@MainActor @Observable
final class ProxyAssistantModel {
    enum Client: String, CaseIterable, Identifiable { case verge, v2rayn, party, generic, mihomo, singbox; var id: String { rawValue } }
    var client: Client?
    var format = ProxyAssistantFormat.clashScript
    var preview: ProxyAssistantPreview?
    var feedback: String?
    var hasBackup = false
    var selecting = false
    private let files: ProxyAssistantFiles
    private let readOnly: Bool
    private var panel: NSOpenPanel?
    private var revision = 0
    init(readOnly: Bool) {
        self.readOnly = readOnly
        files = ProxyAssistantFiles(directory: ProxyAssistantFiles.defaultDirectory(), readOnly: readOnly)
        if !readOnly { hasBackup = files.hasBackup }
    }
    func invalidate() {
        revision += 1; panel?.cancel(nil); panel = nil; selecting = false; preview = nil; feedback = nil
    }
    func selectFile() {
        guard !readOnly, !selecting else { return }
        invalidate(); selecting = true
        let generation = revision, chosenFormat = format
        let panel = NSOpenPanel(); self.panel = panel
        panel.canChooseDirectories = false; panel.canChooseFiles = true; panel.allowsMultipleSelection = false
        panel.resolvesAliases = false; panel.allowedContentTypes = [.plainText, .json, .yaml, .javaScript]
        panel.allowsOtherFileTypes = true
        panel.begin { [weak self] result in
            guard let self, self.revision == generation else { return }
            self.panel = nil; self.selecting = false
            guard result == .OK, let url = panel.url else { return }
            let access = url.startAccessingSecurityScopedResource(); defer { if access { url.stopAccessingSecurityScopedResource() } }
            do { self.preview = try self.files.preview(url, format: chosenFormat); self.feedback = self.preview?.patch.changes.isEmpty == true ? "proxyAssistant.unchanged" : nil }
            catch { self.failure(error) }
        }
    }
    func save() {
        guard !readOnly, let preview else { return }
        do { try files.save(preview); self.preview = nil; feedback = "proxyAssistant.saved" }
        catch { failure(error) }
        hasBackup = files.hasBackup
    }
    func restore() {
        guard !readOnly else { return }
        preview = nil
        do { try files.restore(); feedback = "proxyAssistant.restored" } catch { failure(error) }
        hasBackup = files.hasBackup
    }
    func discard() {
        guard !readOnly else { return }
        preview = nil
        do { try files.discardBackup(); feedback = "proxyAssistant.backupDiscarded" } catch { failure(error) }
        hasBackup = files.hasBackup
    }
    private func failure(_ error: Error) { feedback = "proxyAssistant.error." + ((error as? ProxyAssistantFailure)?.token ?? "unavailable") }
}

struct ProxyAssistantView: View {
    let readOnly: Bool
    @EnvironmentObject private var app: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var model: ProxyAssistantModel
    @StateObject private var runner = CompatibilityRunner()
    @State private var proxy = ProxyRules.summarize(ProxyRules.readSystem())
    @State private var toolToken: AppModel.CampusDirectToolToken?
    @State private var advanced = false
    @State private var choosingClient = false
    @State private var contentHeight: CGFloat = 0
    @State private var headerHeight: CGFloat = 0
    @State private var tunSteps = false
    @State private var manualRules = false
    @State private var copied: String?
    @State private var confirmation: Confirmation?
    private enum Confirmation: String, Identifiable { case restore, discard; var id: String { rawValue } }
    init(readOnly: Bool) {
        self.readOnly = readOnly
        _model = State(initialValue: ProxyAssistantModel(readOnly: readOnly))
    }
    private var fileEditorVisible: Bool {
        !choosingClient && advanced && (model.client == .mihomo || model.client == .singbox || model.client == .verge)
    }
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(SharedCopy.t("proxyAssistant.title")).font(AppFont.title.weight(.semibold))
                Spacer()
                Button(action: close) { AppIcon(name: "x", size: 16).frame(width: 32, height: 32).contentShape(Rectangle()) }
                    .buttonStyle(.plain).help(SharedCopy.t("proxyAssistantMac.close"))
                    .accessibilityLabel(SharedCopy.t("proxyAssistantMac.close"))
                    .accessibilityIdentifier("proxyAssistant.close")
            }.padding(SharedTokens.spaceL)
                .background(GeometryReader { geometry in
                    Color.clear.preference(key: ProxyAssistantHeaderHeightKey.self, value: geometry.size.height)
                })
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: SharedTokens.spaceL) {
                    Text(SharedCopy.t("proxyAssistant.intro"))
                    clientSelection
                    if let client = model.client, !choosingClient, client != .mihomo && client != .singbox {
                        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                            Text(SharedCopy.t("proxyAssistant.stepsTitle")).font(AppFont.headline)
                                .accessibilityAddTraits(.isHeader)
                            clientInstructions(client)
                            if client == .verge {
                                DisclosureGroup(isExpanded: $tunSteps) {
                                    Text(SharedCopy.t("proxyAssistant.verge.tunDetail"))
                                        .foregroundStyle(Color.token("textSecondary"))
                                } label: {
                                    Text(SharedCopy.t("proxyAssistant.verge.tunSteps"))
                                        .accessibilityIdentifier("proxyAssistant.tunSteps")
                                }.disclosureGroupStyle(FullRowDisclosureStyle())
                            }
                        }.accessibilityIdentifier("proxyAssistant.steps")
                    }
                    DisclosureGroup(isExpanded: $advanced) {
                        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                            clientButton(.mihomo)
                            clientButton(.singbox)
                            if fileEditorVisible { advancedOptions }
                            if fileEditorVisible && model.format != .singBox {
                                DisclosureGroup(isExpanded: $manualRules) {
                                    Text(SharedCopy.t("compat.rules.detail")).foregroundStyle(Color.token("textSecondary"))
                                    Text(ProxyRules.rulesText).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                                    Button(SharedCopy.t(copied == "rules" ? "compat.rules.copied" : "compat.rules.copy")) {
                                        Clipboard.copy(ProxyRules.rulesText); copied = "rules"
                                    }.accessibilityIdentifier("proxyAssistant.copyRules")
                                } label: {
                                    Text(SharedCopy.t("proxyAssistant.manualRules"))
                                        .accessibilityIdentifier("proxyAssistant.manualRules")
                                }.disclosureGroupStyle(FullRowDisclosureStyle())
                            }
                            if fileEditorVisible && !model.hasBackup { backupControls }
                        }.padding(.top, SharedTokens.spaceS)
                    } label: {
                        Text(SharedCopy.t("proxyAssistant.advanced"))
                            .accessibilityIdentifier("proxyAssistant.advanced")
                    }.disclosureGroupStyle(FullRowDisclosureStyle())
                    if let feedback = model.feedback {
                        Text(SharedCopy.t(feedback)).foregroundStyle(Color.token(feedback.contains(".error.") ? "warning" : "textPrimary"))
                            .accessibilityIdentifier("proxyAssistant.feedback")
                    }
                    Card(padding: SharedTokens.spaceM) {
                        HStack(spacing: SharedTokens.spaceS) {
                            Text(SharedCopy.t("proxyAssistant.checkTitle")).font(AppFont.headline)
                                .accessibilityAddTraits(.isHeader)
                            InfoButton(topic: "addressResolution")
                        }
                        diagnosisControls
                    }.accessibilityIdentifier("proxyAssistant.checkSection")
                    if model.hasBackup { backupControls }
                    if readOnly { Text(SharedCopy.t("proxyAssistantMac.readonly")).font(AppFont.caption) }
                }
                .font(AppFont.callout).fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading).padding(SharedTokens.spaceL)
                .background(GeometryReader { geometry in
                    Color.clear.preference(key: ProxyAssistantContentHeightKey.self, value: geometry.size.height)
                })
            }
        }
        .frame(minWidth: 440, idealWidth: 500, maxWidth: 560)
        .frame(height: ProxyAssistantLayout.sheetHeight(content: contentHeight, header: headerHeight,
            screen: NSScreen.main?.visibleFrame.height ?? 760))
        .onPreferenceChange(ProxyAssistantContentHeightKey.self) { contentHeight = $0 }
        .onPreferenceChange(ProxyAssistantHeaderHeightKey.self) { headerHeight = $0 }
        .background(Color.token("background"))
        .onExitCommand(perform: close)
        .task {
            while !Task.isCancelled && !app.isMaintaining {
                let latest = ProxyRules.summarize(ProxyRules.readSystem())
                guard latest == proxy else { close(); return }
                do { try await Task.sleep(for: .seconds(5)) } catch { return }
            }
        }
        .onChange(of: runner.running) { wasRunning, running in
            guard wasRunning, !running, let token = toolToken else { return }
            toolToken = nil
            let result = runner.input?.campusDirect ?? "notRun"
            app.finishCampusDirectToolCheck(token, campusPassed: result == "reachable",
                cancelled: app.isPreview || ["cancelled", "notRun"].contains(result))
        }
        .onChange(of: app.networkRevision) { _, _ in close() }
        .onChange(of: app.tunnelActive) { _, _ in close() }
        .onChange(of: app.isMaintaining) { _, maintaining in if maintaining { close() } }
        .onDisappear { model.invalidate(); cancelDiagnosis() }
        .sheet(item: $confirmation) { action in
            AppConfirmationSheet(title: SharedCopy.t(action == .restore ? "proxyAssistant.restoreTitle" : "proxyAssistant.discardTitle"), height: 270,
                                 closeIdentifier: "proxyAssistant.confirm.close", close: { confirmation = nil }) {
                Text(SharedCopy.t(action == .restore ? "proxyAssistant.restoreDetail" : "proxyAssistant.discardDetail"))
                    .fixedSize(horizontal: false, vertical: true)
            } actions: {
                Button(SharedCopy.t("action.cancel")) { confirmation = nil }.keyboardShortcut(.cancelAction)
                Button(SharedCopy.t(action == .restore ? "proxyAssistant.restore" : "proxyAssistant.discardTitle")) {
                    cancelDiagnosis()
                    if action == .restore { model.restore() } else { model.discard() }
                    confirmation = nil
                }.buttonStyle(.borderedProminent)
            }
        }
    }

    @ViewBuilder private var clientSelection: some View {
        if let client = model.client, !choosingClient {
            ProxyAssistantSelectedClientRow(title: SharedCopy.t("proxyAssistant.format." + client.rawValue)) {
                choosingClient = true
            }
        } else {
            VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                Text(SharedCopy.t("proxyAssistant.client")).font(AppFont.headline)
                    .accessibilityAddTraits(.isHeader)
                VStack(spacing: 0) {
                    clientButton(.verge)
                    Divider()
                    clientButton(.v2rayn)
                    Divider()
                    clientButton(.party)
                    Divider()
                    clientButton(.generic)
                }
                .background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium))
            }
        }
    }
    private func clientButton(_ client: ProxyAssistantModel.Client) -> some View {
        Button { selectClient(client) } label: {
            HStack(spacing: SharedTokens.spaceS) {
                Text(SharedCopy.t("proxyAssistant.format." + client.rawValue))
                    .font(AppFont.callout.weight(model.client == client ? .semibold : .regular))
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
                AppIcon(name: model.client == client ? "check" : "chevron-right", size: 16,
                        color: model.client == client ? .token("accent") : .token("textSecondary"))
            }
            .frame(maxWidth: .infinity, minHeight: 40, alignment: .leading)
            .padding(.horizontal, SharedTokens.spaceM)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain).accessibilityIdentifier("proxyAssistant.client." + client.rawValue)
        .accessibilityAddTraits(model.client == client ? .isSelected : [])
    }
    private func selectClient(_ client: ProxyAssistantModel.Client) {
        choosingClient = false
        guard client != model.client else { return }
        model.invalidate(); cancelDiagnosis()
        advanced = client == .mihomo || client == .singbox; tunSteps = false
        manualRules = false; copied = nil; confirmation = nil
        model.client = client
        model.format = client == .mihomo ? .mihomo : client == .singbox ? .singBox : .clashScript
    }
    @ViewBuilder private func clientInstructions(_ client: ProxyAssistantModel.Client) -> some View {
        if client == .generic {
            Text(SharedCopy.t("proxyAssistant.generic.scope")).foregroundStyle(Color.token("textSecondary"))
            Card {
                Text(SharedCopy.t("proxyAssistant.generic.step1"))
                Text(SharedCopy.t("proxyAssistant.generic.step2"))
                fixedTargets(generic: true)
                Text(SharedCopy.t("proxyAssistant.generic.step3"))
            }
            Text(SharedCopy.t("proxyAssistant.generic.limit")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
        } else if client == .v2rayn {
            Text(SharedCopy.t("proxyAssistant.v2rayn.scope")).foregroundStyle(Color.token("textSecondary"))
            Card {
                Text(SharedCopy.t("proxyAssistant.v2rayn.step1"))
                Button(SharedCopy.t(copied == "v2rayn" ? "action.copied" : "proxyAssistant.v2rayn.copy")) {
                    Clipboard.copy(ProxyAssistantConfig.v2raynRules); copied = "v2rayn"
                }.accessibilityIdentifier("proxyAssistant.v2rayn.copyRules")
                Text(SharedCopy.t("proxyAssistant.v2rayn.step2"))
                Text(SharedCopy.t("proxyAssistant.v2rayn.step3"))
            }
            Text(SharedCopy.t("proxyAssistant.v2rayn.limit")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
        } else if client == .verge || client == .party {
            Card {
                if client == .party { Text(SharedCopy.t("proxyAssistant.party.scope")).foregroundStyle(Color.token("textSecondary")) }
                Text(SharedCopy.t("proxyAssistant." + client.rawValue + ".step1"))
                Text(SharedCopy.t("proxyAssistant." + client.rawValue + ".step2"))
                fixedTargets(generic: false)
                Text(SharedCopy.t("proxyAssistant." + client.rawValue + ".step3"))
            }
        }
        if client == .verge || client == .v2rayn || client == .party || client == .generic {
            Text(SharedCopy.t("proxyAssistant.ruleMode")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
        }
    }
    private func fixedTargets(generic: Bool) -> some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            ProxyAssistantTargetRow(text: SharedCopy.t(generic ? "proxyAssistant.generic.domain" : "proxyAssistant.verge.domain"),
                buttonTitle: SharedCopy.t(copied == "domain" ? "action.copied" : "proxyAssistant.generic.copyDomain"),
                copied: copied == "domain", identifier: "proxyAssistant.copyDomain") {
                    Clipboard.copy(ProxyAssistantConfig.domain); copied = "domain"
                }
            ProxyAssistantTargetRow(text: SharedCopy.t(generic ? "proxyAssistant.generic.address" : "proxyAssistant.verge.address"),
                buttonTitle: SharedCopy.t(copied == "address" ? "action.copied" : generic ? "proxyAssistant.generic.copyAddress" : "proxyAssistant.party.copyAddress"),
                copied: copied == "address", identifier: "proxyAssistant.copyAddress") {
                    Clipboard.copy(generic ? "172.30.255.42" : ProxyAssistantConfig.address); copied = "address"
                }
        }
    }
    private var advancedOptions: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            if model.format == .clashScript {
                Text(SharedCopy.t("proxyAssistant.script.step1"))
                Text(SharedCopy.t("proxyAssistant.script.step2"))
                Text(SharedCopy.t("proxyAssistant.script.step3"))
                Button(SharedCopy.t(copied == "script" ? "action.copied" : "proxyAssistant.copy")) {
                    Clipboard.copy(ProxyAssistantConfig.script); copied = "script"
                }.accessibilityIdentifier("proxyAssistant.copyScript")
                Text(SharedCopy.t("proxyAssistant.copyDetail")).font(AppFont.caption)
                Text(SharedCopy.t("proxyAssistant.scriptFile")).font(AppFont.caption)
            } else {
                Text(SharedCopy.t("proxyAssistant.guide." + (model.format == .mihomo ? "mihomo" : "singbox")))
                Text(SharedCopy.t("proxyAssistantMac.formatLimit")).font(AppFont.caption)
            }
            Text(SharedCopy.t("proxyAssistant.scope")).font(AppFont.caption)
            Button(SharedCopy.t("proxyAssistant.select")) { cancelDiagnosis(); model.selectFile() }
                .disabled(readOnly || app.busy || app.isMaintaining || model.selecting || model.hasBackup).accessibilityIdentifier("proxyAssistant.select")
            if let preview = model.preview {
                Text(SharedCopy.f("proxyAssistant.file", ["name": preview.url.lastPathComponent])).textSelection(.enabled)
                Text(SharedCopy.t("proxyAssistantMac.previewPrivacy")).font(AppFont.caption)
                if !preview.patch.changes.isEmpty {
                    Text(SharedCopy.t("proxyAssistant.preview")).font(AppFont.callout.weight(.semibold))
                    ForEach(preview.patch.changes, id: \.self) { change in
                        Label(SharedCopy.t("proxyAssistant.change." + change), systemImage: "checkmark")
                    }
                    Button(SharedCopy.t("proxyAssistant.save")) { cancelDiagnosis(); model.save() }
                        .disabled(readOnly || app.busy || app.isMaintaining || model.hasBackup).buttonStyle(.borderedProminent).accessibilityIdentifier("proxyAssistant.save")
                }
            }
            Text(SharedCopy.t("proxyAssistant.reload." + (model.format == .clashScript ? "verge" : model.format == .mihomo ? "mihomo" : "singbox")))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
        }.padding(.top, SharedTokens.spaceS)
    }
    private var backupControls: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            Text(SharedCopy.t("proxyAssistant.backup")).font(AppFont.caption)
            Text(SharedCopy.t("proxyAssistantMac.backupLocal")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            if model.hasBackup {
                Button(SharedCopy.t("proxyAssistant.restore")) { confirmation = .restore }
                    .disabled(readOnly || app.busy || app.isMaintaining).accessibilityIdentifier("proxyAssistant.restore")
                Button(SharedCopy.t("proxyAssistant.discard")) { confirmation = .discard }
                    .disabled(readOnly || app.busy || app.isMaintaining).accessibilityIdentifier("proxyAssistant.discard")
            }
        }
    }
    private var diagnosisControls: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            Button(SharedCopy.t(runner.running ? "action.stopDiagnosis" : "proxyAssistant.test")) {
                if runner.running { cancelDiagnosis() } else { model.invalidate(); runDiagnosis() }
            }
            .buttonStyle(.borderedProminent)
            .disabled(!runner.running && (app.busy || app.isMaintaining))
            .accessibilityIdentifier("proxyAssistant.test")
            Text(SharedCopy.t("proxyAssistant.testDetail")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            if runner.running {
                HStack(spacing: SharedTokens.spaceS) {
                    ProgressView()
                    Text(SharedCopy.t("proxyAssistant.testRunning"))
                }.accessibilityIdentifier("proxyAssistant.test.feedback")
            } else if runner.input != nil {
                diagnosisResult
            }
        }
    }
    private var diagnosisResult: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            if let input = runner.input {
                let results = ProxyAssistantCheckPresentation(campusDirect: input.campusDirect, campusSystem: input.campusSystem,
                    publicDirect: input.publicDirect, publicSystem: input.publicSystem)
                ProxyAssistantCheckTable(results: results)
                if let verdict = runner.verdict {
                    Text(SharedCopy.t("compat.verdict.\(verdict.verdict).detail"))
                        .fixedSize(horizontal: false, vertical: true)
                        .accessibilityIdentifier("proxyAssistant.test.advice")
                }
                if results.hasSingleTimeout {
                    Text(SharedCopy.t("proxyAssistant.singleTimeoutHint"))
                        .foregroundStyle(Color.token("textSecondary")).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }
    private func runDiagnosis() {
        guard !runner.running, !app.busy, !app.isMaintaining else { return }
        proxy = ProxyRules.summarize(ProxyRules.readSystem())
        toolToken = app.beginCampusDirectToolCheck()
        runner.run(proxy: proxy.state, tunnel: app.tunnelActive ? "detected" : "notDetected",
                   preview: app.isPreview, physicalCheck: app.campusDirectProbe())
    }
    private func cancelDiagnosis() {
        toolToken = nil
        runner.reset()
        app.cancelCampusDirectToolChecks()
    }
    private func close() { model.invalidate(); cancelDiagnosis(); confirmation = nil; dismiss() }
}

private struct ProxyAssistantContentHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}
private struct ProxyAssistantHeaderHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}
#endif
