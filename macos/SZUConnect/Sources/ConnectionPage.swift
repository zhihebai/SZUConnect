import SwiftUI

/// Both connection summaries edit the same preference through their caller's binding.
/// Selecting a mode only changes the next detection; it never starts a request.
struct ProbeModeSelection: View {
    @Binding var selection: ConnectivityProbeMode
    let accessibilityIdentifier: String

    var body: some View {
        HStack(spacing: SharedTokens.spaceS) {
            Picker("", selection: $selection) {
                Text(SharedCopy.t("probe.quick.title")).tag(ConnectivityProbeMode.quick)
                Text(SharedCopy.t("probe.complete.title")).tag(ConnectivityProbeMode.complete)
            }
            .pickerStyle(.segmented).labelsHidden().frame(width: 190, alignment: .leading)
            .accessibilityLabel(SharedCopy.t("probe.label"))
            .accessibilityIdentifier(accessibilityIdentifier)
            InfoButton(topic: "probe")
        }
        .fixedSize(horizontal: true, vertical: false)
    }
}

/// Status and recovery controls, followed by account, website and network settings.
/// Supplemental help uses popovers; errors, pauses, unsaved changes and safety reasons remain visible.
struct ConnectionPage: View {
    @EnvironmentObject private var model: AppModel
    let open: (DashboardDestination) -> Void
    @Binding var focusRequest: DashboardFocusRequest?
    @ObservedObject var networkDetails: NetworkDetailsPresentationState
    @State private var newWebsite = ""
    @State private var websitePresetFeedback = WebsitePresetFeedback()
    @State private var showPassword = false
    @State private var forms = ConnectionFormDisclosureState()
    @State private var selectedWebsiteResult: String?
    @State private var partialLoginConfirmation: AppModel.ForcedLoginConfirmation?
    @State private var identificationCopied = false
    @State private var showingProxyAssistant = false
    private enum CredentialField: Hashable { case account, password }
    @FocusState private var credentialFocus: CredentialField?
    @FocusState private var websiteFocused: Bool

    var body: some View {
        ScrollViewReader { reader in
            PageScroll(title: "", maxWidth: SharedTokens.sizePanelMaxWidth) {
                statusHero.tourTarget(.status).id("status")
                if let error = model.manualNetworkReadError {
                    Banner(tone: "warning", title: error).accessibilityIdentifier("network.manualReadError")
                }
                #if os(macOS)
                if model.campusDirectHelpSuggested { campusDirectRecommendation }
                #endif
                ConnectionSetupView(open: open, openNetworkSettings: { focusRequest = DashboardFocusRequest(section: "guard") })
                recoveryCard.tourTarget(.recovery).id("guard")
                accountCard.tourTarget(.account).id("account")
                websitesCard.tourTarget(.websites).id("websites")
                networkRow.tourTarget(.network).id("network")
            }
            .onChange(of: focusRequest) { _, request in focus(request, reader: reader) }
            .onAppear { focus(focusRequest, reader: reader) }
        }
        .modifier(PartialLoginWarning(confirmation: $partialLoginConfirmation))
        #if os(macOS)
        .sheet(isPresented: $showingProxyAssistant) {
            ProxyAssistantView(readOnly: model.sessionReadOnly || model.isPreview)
        }
        #endif
        .onChange(of: networkDetails.expanded) { _, expanded in
            if !expanded { showingProxyAssistant = false }
        }
        .onChange(of: model.networkRevision) { _, _ in showingProxyAssistant = false }
        .onChange(of: model.tunnelActive) { _, _ in showingProxyAssistant = false }
        .onChange(of: model.isMaintaining) { _, maintaining in
            if maintaining { showingProxyAssistant = false }
        }
        .onChange(of: model.customWebsiteTargets.count) { _, count in
            if count >= WebsiteTarget.maximumCount { websiteFocused = false }
        }
        .onChange(of: model.hasSavedCredentials) { _, _ in credentialStateChanged() }
        .onChange(of: model.hasUnsavedCredentialChanges) { _, _ in credentialStateChanged() }
        .onDisappear {
            showPassword = false
            credentialFocus = nil
            websiteFocused = false
            showingProxyAssistant = false
            partialLoginConfirmation = nil
            model.cancelPendingManualNetworkRead()
        }
    }

    private func openCampusDirectAssistant() {
        guard !model.isMaintaining else { return }
        showingProxyAssistant = true
    }

    #if os(macOS)
    private var campusDirectRecommendation: some View {
        Card(padding: SharedTokens.spaceM) {
            HStack(spacing: SharedTokens.spaceS) {
                AppIcon(name: "info", size: 18, color: .tone("warning"))
                Text(SharedCopy.t("proxyAssistant.offline.title"))
                    .font(AppFont.callout.weight(.semibold)).accessibilityAddTraits(.isHeader)
            }
            Text(SharedCopy.t(model.physicalNetworkIdentified ? "proxyAssistant.offline.detail" : "proxyAssistant.offline.unidentified"))
                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("proxy.suggestion.detail")
            ActionButton(title: SharedCopy.t("proxyAssistant.offline.action"), icon: "settings", action: openCampusDirectAssistant)
                .accessibilityIdentifier("proxy.suggestion.open")
        }
    }
    #endif

    private func focus(_ request: DashboardFocusRequest?, reader: ScrollViewProxy) {
        guard let request else { return }
        Task { @MainActor in
            guard focusRequest?.id == request.id else { return }
            if request.section == "network" { networkDetails.reveal() }
            forms.reveal(request.section)
            await Task.yield()
            guard focusRequest?.id == request.id else { return }
            reader.scrollTo(request.section, anchor: .top)
            if request.section == "account" { credentialFocus = .account }
            if request.section == "websites" { websiteFocused = model.customWebsiteTargets.count < WebsiteTarget.maximumCount }
            focusRequest = nil
        }
    }

    // MARK: status hero

    private var statusHero: some View {
        let p = model.presentation
        return Card(padding: SharedTokens.spaceXl) {
            HStack(alignment: .center, spacing: 16) {
                ZStack {
                    Circle().fill(Color.toneSurface(p.tone)).frame(width: 56, height: 56)
                    if model.isForegroundBusy || model.systemPathIsChecking { ProgressView() }
                    else { AppIcon(name: SharedTokens.tones[p.tone]?.icon ?? "info", size: 28, color: .tone(p.tone)) }
                }
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 4) {
                        Text(SharedCopy.t(p.headline)).font(AppFont.hero).accessibilityAddTraits(.updatesFrequently)
                        InfoButton(topic: "status")
                    }
                    Text(SharedCopy.t(p.detail)).font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 10) { heroActions(p) }
                VStack(alignment: .leading, spacing: 8) { heroActions(p) }
            }
            if p.banner?.key == "banner.loginRejected" {
                Banner(tone: "warning", title: SharedCopy.t("banner.loginRejected.title"),
                       detail: SharedCopy.t("banner.loginRejected.detail"))
            }
            Divider()
            FlowLayout(spacing: 8) {
                checkChip("Apple", endpoint: model.displayedConnectivity?.appleResult)
                checkChip("Microsoft", endpoint: model.displayedConnectivity?.microsoftResult)
                ForEach(model.customWebsiteTargets.filter(\.enabled)) { target in websiteChip(target) }
            }
            VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                probeFooterText.fixedSize(horizontal: false, vertical: true)
                probeSelection
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var probeFooterText: some View {
        Text(footerText).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
    }

    private var probeSelection: some View {
        ProbeModeSelection(
            selection: Binding(get: { model.probeMode }, set: { model.setProbeMode($0) }),
            accessibilityIdentifier: "connection.probeMode"
        )
    }

    private var footerText: String {
        var parts = [SharedCopy.t(model.systemPathIsLeading || model.systemPathIsChecking ? "env.systemProbe" : "env.physicalProbe"), model.connectionKindTitle]
        if model.systemProxyActive { parts.append(SharedCopy.t("dashboard.proxy")) }
        if model.tunnelActive { parts.append(SharedCopy.t("dashboard.tunnel")) }
        if model.displayedConnectivity != nil,
           let date = model.systemPathIsLeading ? model.systemConnectivityCheckedAt : model.lastCheckedAt {
            let mode = model.displayedConnectivity?.mode ?? model.lastProbeMode
            parts.append((mode?.title ?? "") + " " + date.formatted(date: .omitted, time: .standard))
        }
        return parts.joined(separator: " · ")
    }

    /// One primary action, one quiet secondary: both come from the shared status presenter.
    @ViewBuilder private func heroActions(_ p: StatusPresentation) -> some View {
        if model.isForegroundBusy {
            ActionButton(title: SharedCopy.t("action.cancel"), icon: "x", kind: .primary, isDefault: true) { model.cancelCurrentOperation() }
        } else {
            if let primary = p.primary { statusAction(primary, prominent: true) }
            if let secondary = p.secondary { statusAction(secondary, prominent: false) }
        }
    }

    @ViewBuilder private func statusAction(_ token: String, prominent: Bool) -> some View {
        let kind: AppButtonKind = prominent ? .primary : .secondary
        switch token {
        case "check":
            ActionButton(title: SharedCopy.t("action.check"), icon: "refresh", kind: kind, isDefault: prominent) { model.checkConnection() }
        case "login":
            ActionButton(title: SharedCopy.t("action.login"), icon: "log-in", kind: kind, isDefault: prominent) { model.connect() }
        case "partialLogin":
            ActionButton(title: SharedCopy.t("action.partialLogin"), icon: "log-in", kind: kind, isDefault: prominent) { model.connect() }
        case "forceLogin":
            ActionButton(title: SharedCopy.t("action.forceLogin"), icon: "log-in", kind: kind, isDefault: prominent) {
                partialLoginConfirmation = model.forcedLoginConfirmation()
            }
        case "cancel":
            ActionButton(title: SharedCopy.t("action.cancel"), icon: "x", kind: kind, isDefault: prominent) { model.cancelCurrentOperation() }
        case "resume":
            ActionButton(title: SharedCopy.t("action.resume"), icon: "play", kind: kind, isDefault: prominent) { model.resumeAutomaticRecovery() }
        case "trust":
            ActionButton(title: SharedCopy.t("setup.memory.action"), icon: "shield-check", kind: kind, isDefault: prominent) {
                focusRequest = DashboardFocusRequest(section: "guard")
            }
        case "account":
            ActionButton(title: SharedCopy.t("action.editAccount"), icon: "user", kind: kind, isDefault: prominent) { open(.account) }
        case "area":
            ActionButton(title: SharedCopy.t("account.areaTitle"), icon: "globe", kind: kind, isDefault: prominent) { open(.account) }
                .accessibilityIdentifier("campus.selectArea")
        case "network":
            ActionButton(title: SharedCopy.t("action.viewNetwork"), icon: "globe", kind: kind, isDefault: prominent) { open(.network) }
        default:
            EmptyView()
        }
    }

    private func checkChip(_ name: String, endpoint: ConnectivityEndpointResult?) -> some View {
        let active = model.systemPathIsChecking || (model.isForegroundBusy && !model.isVerifyingMemory)
        let success = endpoint?.succeeded == true
        let notWaited = model.displayedConnectivity?.mode == .quick && endpoint?.state == .notCompleted
        let tone = active ? "busy" : endpoint == nil ? "neutral" : success ? "success" : endpoint?.state == .notCompleted ? "neutral" : "warning"
        let text = active ? SharedCopy.t("dashboard.checking")
            : notWaited ? SharedCopy.t("dashboard.notWaited")
            : (endpoint?.summary ?? SharedCopy.t("compat.result.notRun"))
        return resultChip(name, text: text, tone: tone)
    }

    @ViewBuilder private func websiteChip(_ target: WebsiteTarget) -> some View {
        let result = model.lastInternetCheck?.websites.first(where: { $0.targetID == target.id })?.result
        let skipped = model.lastInternetCheck?.base.mode == .quick
        let active = model.isForegroundBusy && !model.isVerifyingMemory
        let text = active ? SharedCopy.t("dashboard.checking") : result?.title ?? SharedCopy.t(skipped ? "dashboard.skipped" : "compat.result.notRun")
        let tone = active ? "busy" : result == nil ? "neutral" : result?.state == .accessible ? "success" : "warning"
        if let result, !active, !model.websiteConfigurationChanged {
            Button { selectedWebsiteResult = target.id } label: {
                resultChip(target.displayName, text: text, tone: tone, showsDetails: true)
            }
            .buttonStyle(.plain)
            .help(SharedCopy.t("website.result.details"))
            .accessibilityLabel(target.displayName + "，" + text + "，" + SharedCopy.t("website.result.details"))
            .accessibilityIdentifier("website.result." + target.id)
            .popover(isPresented: websiteResultIsPresented(for: target.id), arrowEdge: .bottom) {
                WebsiteResultDetails(name: target.displayName, result: result, close: { selectedWebsiteResult = nil })
            }
        } else {
            resultChip(target.displayName, text: text, tone: tone)
        }
    }

    private func websiteResultIsPresented(for targetID: String) -> Binding<Bool> {
        Binding<Bool>(
            get: { selectedWebsiteResult == targetID },
            set: { isPresented in
                if !isPresented { selectedWebsiteResult = nil }
            }
        )
    }

    private func resultChip(_ name: String, text: String, tone: String, showsDetails: Bool = false) -> some View {
        HStack(spacing: 6) {
            AppIcon(name: SharedTokens.tones[tone]?.icon ?? "info", size: 14, color: .tone(tone))
            Text(name).font(AppFont.caption.weight(.semibold)).lineLimit(1).truncationMode(.middle)
            Text(text).font(AppFont.caption).foregroundStyle(Color.token("textSecondary")).lineLimit(1)
            if showsDetails { AppIcon(name: "info", size: 12, color: .token("textSecondary")) }
        }
        .padding(.horizontal, 10)
        .frame(minHeight: SharedTokens.sizeChipHeight)
        .frame(maxWidth: 300)
        .background(Color.token("surfaceMuted"), in: Capsule())
        .accessibilityElement(children: .combine)
    }

    // MARK: auto recovery

    private var recoveryCard: some View {
        let readiness = model.readiness
        let p = model.presentation
        let recover = model.monitoringMode == .recover && !model.sessionReadOnly
        let badge: (text: String, tone: String)? = recover ? (SharedCopy.t(readiness.title), readiness.tone) : nil
        return DashboardSection(title: SharedCopy.t("dashboard.guard"), icon: "shield-check", info: "auto", badge: badge) {
            Picker(SharedCopy.t("guard.modeTitle"), selection: Binding(get: { model.monitoringMode }, set: { model.setMonitoringMode($0) })) {
                ForEach(MonitoringMode.allCases) { Text($0.title).tag($0) }
            }.pickerStyle(.segmented).labelsHidden().disabled(model.sessionReadOnly)
            Text(SharedCopy.t("env.campusVerification." + model.campusVerificationDisplayState))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            if model.sessionReadOnly {
                Text(SharedCopy.t("settings.readOnlyLaunch")).font(AppFont.caption).foregroundStyle(Color.tone("warning"))
            } else if !recover {
                Text(model.monitoringMode.caption).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            } else {
                if let target = model.authenticationTargetName {
                    Text(SharedCopy.f("env.authenticationTarget", ["name": target]))
                        .font(AppFont.callout).fontWeight(.semibold)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if model.tunnelActive && model.systemPhysicalLabel != nil && model.authenticationTargetName == nil {
                    Text(SharedCopy.t("env.tunnelSeparate"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                    #if os(macOS)
                    Text(SharedCopy.t("env.campusPhysicalChannel"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                    #endif
                }
                FlowLayout(spacing: 8) {
                    ForEach(readiness.items) { item in
                        ToneLabel(text: SharedCopy.t(item.key), tone: item.tone)
                    }
                }
                if model.networkRemembered, ["ready", "awaitingProof"].contains(model.autoLoginKey), model.memoryVerificationMessage == nil {
                    Text(SharedCopy.t(model.campusVerificationHintKey))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
                reasonBlock(p)
            }
            if model.physicalIdentificationNeeded {
                physicalIdentificationNotice
            }
            if !model.sessionReadOnly {
                if !model.networkRemembered {
                    Text(SharedCopy.t(model.currentNetworkAuthorized ? "memory.sessionTrusted" : "memory.notRemembered"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack(alignment: .top, spacing: SharedTokens.spaceS) {
                    FlowLayout(spacing: SharedTokens.spaceS) { recoveryActions }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    InfoButton(topic: "memory")
                }
                if let message = model.memoryVerificationMessage {
                    Text(message).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let hint = rememberHint {
                    Text(hint).font(AppFont.caption).foregroundStyle(Color.token("textSecondary")).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private var physicalIdentificationNotice: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            Text(model.physicalIdentificationDetail)
                .font(AppFont.callout).foregroundStyle(Color.tone("warning"))
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("network.identification.reason")
            FlowLayout(spacing: SharedTokens.spaceS) {
                ActionButton(title: SharedCopy.t("env.identification.refresh"), icon: "refresh") {
                    identificationCopied = false
                    model.refreshPhysicalIdentification()
                }
                .disabled(model.networkInformationRefreshing || model.isMaintaining)
                .accessibilityIdentifier("network.identification.refresh")
                ActionButton(title: SharedCopy.t(identificationCopied ? "records.copyDone" : "env.identification.copy"), icon: "copy") {
                    Clipboard.copy(model.diagnosticSummary)
                    identificationCopied = true
                }
                .accessibilityIdentifier("network.identification.copy")
            }
            Text(SharedCopy.t("env.identification.localOnly"))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            if let feedback = model.networkInformationFeedback {
                Text(feedback).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("network.identification.feedback")
            }
        }
    }

    @ViewBuilder private func reasonBlock(_ p: StatusPresentation) -> some View {
        if model.prioritizingCampusRecovery, !model.isOnline, !model.campusVerificationInProgress {
            Text(SharedCopy.t("auto.prioritizeCampus"))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
        }
        if model.entrySelectionNeeded {
            Text(SharedCopy.t("campus.entry.bothDetected"))
                .font(AppFont.caption).foregroundStyle(Color.tone("warning"))
                .fixedSize(horizontal: false, vertical: true)
        }
        if let banner = p.banner, !model.entrySelectionNeeded,
           !["banner.readOnly", "banner.loginRejected"].contains(banner.key),
           !(model.networkRemembered && banner.key == "banner.awaitingProof") {
            Banner(tone: banner.tone, title: SharedCopy.t(banner.key + ".title"), detail: SharedCopy.t(banner.key + ".detail"))
        }
        if !model.proofWaitKey.isEmpty && !model.entrySelectionNeeded && !(model.networkRemembered && ["unconfirmed", "tunnel"].contains(model.proofWaitKey)) {
            #if os(macOS)
            Text(SharedCopy.t(model.proofWaitKey == "tunnel" ? "memory.wait.physicalChannel" : "memory.wait." + model.proofWaitKey))
                .font(AppFont.caption).foregroundStyle(Color.tone("warning"))
                .fixedSize(horizontal: false, vertical: true)
            #else
            Text(SharedCopy.t("memory.wait." + model.proofWaitKey)).font(AppFont.caption).foregroundStyle(Color.tone("warning"))
                .fixedSize(horizontal: false, vertical: true)
            #endif
        }
        if model.cooldownRemaining != nil && !model.isOnline {
            Text(SharedCopy.t("auto.coolingDown.detail")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
        }
    }

    @ViewBuilder private var recoveryActions: some View {
        if model.monitoringMode == .recover && model.hasSavedCredentials {
            if model.pausedForAutomatic {
                ActionButton(title: SharedCopy.t("action.resume"), icon: "play") { model.resumeAutomaticRecovery() }
            } else {
                if model.autoLoginKey == "limited" {
                    ActionButton(title: SharedCopy.t("action.resume"), icon: "play") { model.resumeAutomaticRecovery() }
                }
                ActionButton(title: SharedCopy.t("action.pause"), icon: "pause") { model.pauseAutomaticRecovery() }
            }
        }
        if model.entrySelectionNeeded { statusAction("area", prominent: false) }
        memoryActions
    }

    @ViewBuilder private var memoryActions: some View {
        if model.isVerifyingMemory {
            ActionButton(title: SharedCopy.t("action.cancel"), icon: "x") { model.cancelCurrentOperation() }
        } else if model.physicalIdentificationNeeded {
            EmptyView() // The local identification notice above offers an actionable explanation.
        } else if model.sessionOnlyTrust {
            if model.currentNetworkAuthorized {
                ActionButton(title: SharedCopy.t("action.revokeTrust"), kind: .plain) { model.revokeCurrentNetworkAuthorization() }
            } else {
                ActionButton(title: SharedCopy.t("memory.onceCombination"), icon: "shield-check", kind: .primary) { model.authorizeCurrentNetwork() }
                    .disabled(!model.canTrustCurrentNetwork)
            }
        } else if model.networkRemembered && !model.shouldOfferRememberNetwork {
            ActionButton(title: SharedCopy.t("campus.verify.action"), icon: "shield-check") { model.verifyCurrentCampusService() }
                .disabled(!model.canVerifyCurrentCampusService)
                .accessibilityIdentifier("campus.verify")
            ActionButton(title: SharedCopy.t("memory.forget"), kind: .plain) { model.forgetCurrentNetwork() }
        } else {
            ActionButton(title: SharedCopy.t(model.area == .teaching ? "memory.rememberTeaching" : "memory.remember"), icon: "bookmark") { model.verifyAndRememberCurrentNetwork() }
                .disabled(!model.canVerifyAndRememberNetwork)
                .accessibilityIdentifier("memory.verifyAndRemember")
            if model.currentNetworkAuthorized {
                ActionButton(title: SharedCopy.t("action.revokeTrust"), kind: .plain) { model.revokeCurrentNetworkAuthorization() }
            } else {
                ActionButton(title: SharedCopy.t("memory.once"), kind: .plain) { model.authorizeCurrentNetwork() }
                    .disabled(!model.canTrustCurrentNetwork)
                    .accessibilityIdentifier("memory.trustOnce")
            }
        }
        if model.needsNetworkMemoryUpdate && !model.isVerifyingMemory {
            ActionButton(title: SharedCopy.t("memory.forget"), kind: .plain) { model.forgetCurrentNetwork() }
        }
    }

    private var rememberHint: String? {
        guard !model.canVerifyAndRememberNetwork, !model.busy, model.shouldOfferRememberNetwork else { return nil }
        if !model.canTrustCurrentNetwork && model.networkAvailable { return model.trustUnavailableDetail }
        if model.hasMultipleNetworkAttachments && model.authenticationTargetName == nil { return SharedCopy.t("memory.hint.multiple") }
        guard model.area == .teaching || model.autoLoginKey == "needsTrust" else { return nil }
        switch model.rememberBlocker {
        case "": return nil
        case "unreadable": return SharedCopy.t("memory.unreadable")
        default: return SharedCopy.t("memory.hint." + model.rememberBlocker)
        }
    }

    // MARK: account

    private var accountExpanded: Bool {
        forms.accountExpanded(saved: model.hasSavedCredentials, hasDraft: model.hasUnsavedCredentialChanges)
    }
    private var accountExpansion: Binding<Bool> {
        Binding(get: { accountExpanded }, set: { expanded in
            forms.setAccountExpanded(expanded, saved: model.hasSavedCredentials, hasDraft: model.hasUnsavedCredentialChanges)
            if !accountExpanded { showPassword = false; credentialFocus = nil }
        })
    }
    private func credentialStateChanged() {
        forms.credentialStateChanged(saved: model.hasSavedCredentials, hasDraft: model.hasUnsavedCredentialChanges)
        if !accountExpanded { showPassword = false; credentialFocus = nil }
    }
    private var accountCard: some View {
        Card(padding: SharedTokens.spaceM) {
            HStack(alignment: .top, spacing: SharedTokens.spaceXs) {
                DisclosureHeader(isExpanded: accountExpansion) {
                    AppIcon(name: "user", size: 18, color: .token("accent"))
                    Text(SharedCopy.t("account.title")).font(AppFont.headline).accessibilityAddTraits(.isHeader)
                }.accessibilityIdentifier("account.disclosure")
                InfoButton(topic: "account")
            }
            if !accountExpanded {
                Text(model.savedAccountHint.map { SharedCopy.f("account.saved", ["account": $0]) }
                    ?? SharedCopy.t("dashboard.accountSaved"))
                    .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                    .accessibilityIdentifier("account.savedSummary")
            } else {
                accountEditor
            }
        }
    }
    private var accountEditor: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 8) {
                    accountInput.frame(minWidth: 150)
                    passwordInput.frame(minWidth: 180)
                }
                VStack(alignment: .leading, spacing: 8) {
                    accountInput
                    passwordInput
                }
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 12) {
                    areaSelection
                    Spacer(minLength: 0)
                    accountDraftState
                }
                VStack(alignment: .leading, spacing: 8) {
                    areaSelection
                    accountDraftState
                }
            }
        }
    }

    private var accountInput: some View {
        TextField(SharedCopy.t("account.accountLabel"), text: $model.account, prompt: Text(SharedCopy.t("dashboard.accountPlaceholder")))
            .textContentType(.username).textFieldStyle(.roundedBorder).focused($credentialFocus, equals: .account)
            .disabled(model.sessionReadOnly)
    }

    private var passwordInput: some View {
        HStack(spacing: 4) {
            Group {
                if showPassword { TextField(SharedCopy.t("account.passwordLabel"), text: $model.password) }
                else { SecureField(SharedCopy.t("account.passwordLabel"), text: $model.password) }
            }.textContentType(.password).textFieldStyle(.roundedBorder)
                .focused($credentialFocus, equals: .password)
                .disabled(model.sessionReadOnly)
            Button { showPassword.toggle() } label: {
                AppIcon(name: showPassword ? "eye-off" : "eye", size: 18)
                    .frame(width: 32, height: 32).contentShape(Rectangle())
            }.buttonStyle(.plain).accessibilityLabel(SharedCopy.t(showPassword ? "action.hidePassword" : "action.showPassword"))
        }
    }

    private var areaSelection: some View {
        HStack(spacing: 8) {
            Text(SharedCopy.t("account.areaTitle")).font(AppFont.callout)
            Picker(SharedCopy.t("account.areaTitle"), selection: $model.area) {
                ForEach(CampusArea.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.menu).labelsHidden().frame(width: 124, alignment: .leading)
            .disabled(model.sessionReadOnly)
            InfoButton(topic: "area")
        }
        .fixedSize(horizontal: true, vertical: false)
    }

    private var accountDraftState: some View {
        HStack(spacing: 8) {
            Text(SharedCopy.t(model.hasUnsavedCredentialChanges ? "account.unsaved" : model.hasSavedCredentials ? "dashboard.accountSaved" : "account.notSaved"))
                .font(AppFont.caption)
                .foregroundStyle(model.hasUnsavedCredentialChanges ? Color.tone("warning") : Color.token("textSecondary"))
            if model.hasUnsavedCredentialChanges {
                Button(SharedCopy.t("action.discard")) { model.revertCredentialDraft() }
                Button(SharedCopy.t("action.saveAccount")) { model.saveCredentials() }
                    .disabled(model.sessionReadOnly || !model.hasCompleteDraft)
            }
        }
        .fixedSize(horizontal: true, vertical: false)
    }

    // MARK: websites

    private var websiteExpansion: Binding<Bool> {
        Binding(get: { forms.websitesExpanded }, set: { expanded in
            forms.setWebsitesExpanded(expanded)
            if !expanded { websiteFocused = false }
        })
    }
    private var websitesCard: some View {
        Card(padding: SharedTokens.spaceM) {
            HStack(alignment: .top, spacing: SharedTokens.spaceXs) {
                DisclosureHeader(isExpanded: websiteExpansion) {
                    AppIcon(name: "globe", size: 18, color: .token("accent"))
                    Text(SharedCopy.t("internet.targets.title")).font(AppFont.headline).accessibilityAddTraits(.isHeader)
                }.accessibilityIdentifier("websites.disclosure")
                InfoButton(topic: "websites")
            }
            Text(SharedCopy.f("internet.targets.summary", ["total": String(model.customWebsiteTargets.count), "enabled": String(model.selectedWebsiteCount)]))
                .font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                .accessibilityIdentifier("websites.summary")
            if forms.websitesExpanded { websitesEditor }
        }
    }
    private var websitesEditor: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            if model.probeMode == .quick && model.selectedWebsiteCount > 0 {
                Text(SharedCopy.t("internet.quickSkips")).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            }
            ForEach(model.customWebsiteTargets) { target in
                HStack(spacing: 8) {
                    Toggle(isOn: Binding(get: { target.enabled }, set: { model.setWebsiteEnabled(target.id, enabled: $0) })) {
                        Text(target.url).font(AppFont.callout)
                            .lineLimit(1).truncationMode(.middle)
                            .help(target.url)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    #if os(macOS)
                    .toggleStyle(.checkbox)
                    #endif
                    .disabled(!model.canEditWebsiteTargets)
                    Button { model.removeWebsite(target.id) } label: {
                        AppIcon(name: "x", size: 16).frame(width: 32, height: 32).contentShape(Rectangle())
                    }.buttonStyle(.plain).disabled(!model.canEditWebsiteTargets)
                        .accessibilityLabel(SharedCopy.t("internet.remove") + " " + target.url)
                }.frame(minHeight: 40)
            }
            if model.customWebsiteTargets.count < WebsiteTarget.maximumCount {
                HStack(spacing: 8) {
                    TextField(SharedCopy.t("internet.addLabel"), text: $newWebsite,
                              prompt: Text(SharedCopy.t("internet.addPlaceholder")).foregroundColor(Color.token("textSecondary")))
                        .foregroundStyle(Color.token("textPrimary"))
                        .textFieldStyle(.roundedBorder).focused($websiteFocused).onSubmit(addWebsite)
                    ActionButton(title: SharedCopy.t("internet.add"), icon: "plus", action: addWebsite)
                        .disabled(newWebsite.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !model.canEditWebsiteTargets)
                }
            }
            websitePresetButtons
            Text(SharedCopy.t("internet.targets.detail"))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            Divider()
            VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                HStack(spacing: SharedTokens.spaceXs) {
                    Text(SharedCopy.t("internet.strategy")).font(AppFont.callout.weight(.medium))
                    InfoButton(topic: "websiteStrategy")
                }
                Picker(SharedCopy.t("internet.strategy"), selection: Binding(get: { model.customWebsiteStrategy }, set: { model.setWebsiteStrategy($0) })) {
                    ForEach(WebsiteProbeStrategy.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden().disabled(!model.canEditWebsiteTargets)
                .accessibilityIdentifier("websites.strategy")
                Text(SharedCopy.t("internet.strategyScope"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                Text(model.customWebsiteStrategy.sharedDetail)
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let error = model.websiteConfigurationError {
                Text(error).font(AppFont.caption).foregroundStyle(Color.tone("danger"))
            }
        }
    }

    private var websitePresetButtons: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            Text(SharedCopy.t("internet.presets.title")).font(AppFont.callout.weight(.medium))
            ViewThatFits(in: .horizontal) {
                HStack(spacing: SharedTokens.spaceS) {
                    ForEach(WebsitePresetPresentation.items) { preset in websitePresetButton(preset) }
                }.fixedSize(horizontal: true, vertical: false)
                VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                    ForEach(WebsitePresetPresentation.items) { preset in websitePresetButton(preset) }
                }
            }
            Text(SharedCopy.t("internet.presets.detail"))
                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            if let feedback = websitePresetFeedback.messageKey(contains: { model.hasWebsitePreset(key: $0) }) {
                Text(SharedCopy.t(feedback)).font(AppFont.caption).foregroundStyle(Color.tone("success"))
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("websites.presetFeedback")
            }
        }
    }

    private func websitePresetButton(_ preset: WebsitePresetPresentation.Item) -> some View {
        let added = model.hasWebsitePreset(key: preset.key)
        return ActionButton(title: SharedCopy.t(preset.titleKey(added: added)), icon: added ? "check" : "plus") {
            // Presets are saved unchecked. This path deliberately leaves the
            // manually typed draft, probe mode and current results alone.
            let saved = model.addWebsitePreset(key: preset.key)
            websitePresetFeedback.record(key: preset.key, succeeded: saved)
        }
        .disabled(!model.canAddWebsitePreset(key: preset.key))
        .help(preset.url)
        .accessibilityIdentifier("websites.preset." + preset.key)
    }

    private func addWebsite() {
        if model.addWebsite(newWebsite) { newWebsite = "" }
    }

    // MARK: network environment

    private var networkRow: some View {
        Card(padding: SharedTokens.spaceM) {
            HStack(alignment: .top, spacing: SharedTokens.spaceXs) {
                DisclosureHeader(isExpanded: $networkDetails.expanded) {
                    AppIcon(name: "globe", size: 18, color: .token("accent"))
                    ViewThatFits(in: .horizontal) {
                        HStack(spacing: SharedTokens.spaceS) { networkHeading }
                        VStack(alignment: .leading, spacing: SharedTokens.spaceXs) { networkHeading }
                    }
                }
                .accessibilityIdentifier("network.disclosure")
                InfoButton(topic: "network")
            }
            if networkDetails.expanded {
                Divider()
                NetworkDetailsView(openProxyAssistant: openCampusDirectAssistant)
            }
        }
    }

    @ViewBuilder private var networkHeading: some View {
        Text(SharedCopy.t("dashboard.network")).font(AppFont.headline)
        Text(networkSummary).font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            .fixedSize(horizontal: false, vertical: true)
    }

    private var networkSummary: String {
        var parts = [model.connectionKindTitle]
        if model.systemProxyActive { parts.append(SharedCopy.t("dashboard.proxy")) }
        if model.tunnelActive { parts.append(SharedCopy.t("dashboard.tunnel")) }
        return parts.joined(separator: " · ")
    }
}

private struct DashboardSection<Content: View>: View {
    let title: String
    let icon: String
    var info: String? = nil
    var badge: (text: String, tone: String)? = nil
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                AppIcon(name: icon, size: 18, color: .token("accent"))
                Text(title).font(AppFont.headline).accessibilityAddTraits(.isHeader)
                if let info { InfoButton(topic: info) }
                Spacer(minLength: 8)
                if let badge { ToneLabel(text: badge.text, tone: badge.tone) }
            }
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .background(Color.token("surface"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous).strokeBorder(Color.token("border")))
    }
}


private struct WebsiteResultDetails: View {
    let name: String
    let result: WebsiteProbeResult
    let close: () -> Void

    private var needsRoutingExplanation: Bool {
        ![.accessible, .cancelled, .invalidInput, .securityPolicy].contains(result.state)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            Text(name).font(AppFont.headline).textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
            ScrollView {
                VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                    if let address = result.displayURL {
                        Text(address).font(AppFont.caption).textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Text(result.sharedTitle).font(AppFont.headline)
                    Text(result.sharedDetail).font(AppFont.callout)
                    VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                        Text(SharedCopy.t("website.result.strategy")).font(AppFont.caption.weight(.semibold))
                        Text(result.strategy.title).font(AppFont.callout)
                        Text(result.strategy.sharedDetail).font(AppFont.callout)
                    }
                    if needsRoutingExplanation {
                        Text(SharedCopy.t("internet.routingWarning.title")).font(AppFont.headline)
                        Text(SharedCopy.t("internet.routingWarning.body")).font(AppFont.callout)
                    }
                    Text(SharedCopy.t("website.result.scope")).font(AppFont.callout)
                }
                .foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 360)
            HStack {
                Spacer()
                Button(SharedCopy.t("info.close"), action: close).keyboardShortcut(.cancelAction)
            }
        }
        .padding(SharedTokens.spaceL)
        .frame(width: 340)
    }
}

struct PartialLoginWarning: ViewModifier {
    @EnvironmentObject private var model: AppModel
    @Binding var confirmation: AppModel.ForcedLoginConfirmation?

    private var canContinue: Bool {
        confirmation.map(model.canContinueForcedLogin) ?? false
    }

    func body(content: Content) -> some View {
        let captured = confirmation
        let isPresented = Binding(get: { confirmation != nil }, set: { if !$0 { confirmation = nil } })
        content.alert(SharedCopy.t("internet.partial.loginWarning.title"), isPresented: isPresented) {
            Button(SharedCopy.t("internet.partial.loginWarning.cancel"), role: .cancel) {}
                .keyboardShortcut(.defaultAction)
            Button(SharedCopy.t("internet.partial.loginWarning.continue")) { model.connectEvenIfOnline(confirmation: captured) }
                .disabled(!canContinue)
        } message: {
            Text(SharedCopy.t("internet.partial.loginWarning.body"))
        }
        .onChange(of: canContinue) { _, allowed in
            if !allowed { confirmation = nil }
        }
    }
}
