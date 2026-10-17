import SwiftUI

enum AppPage: String, CaseIterable, Identifiable {
    case connection, records, settings
    var id: String { rawValue }
    var title: String { SharedCopy.t("nav.\(rawValue)") }
    var icon: String { self == .connection ? "signal" : self == .records ? "records" : "settings" }
}
enum AppSheet: String, Identifiable {
    case help, about
    var id: String { rawValue }
}
enum DashboardDestination: String {
    case account, network, wifiRecovery, tutorial, help, about
}
struct DashboardFocusRequest: Equatable {
    let section: String
    let id = UUID()
}

/// Routine controls and the guided tour share the real dashboard. Help/about use sheets.
struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.colorScheme) private var colorScheme
    @AppStorage("appearanceMode") private var appearance = AppearancePreference.system.rawValue
    @State private var previewAppearance = AppearancePreference.system.rawValue
    @State private var page: AppPage = .connection
    @State private var sheet: AppSheet?
    @State private var focusRequest: DashboardFocusRequest?
    @State private var showWhatsNew = false
    @State private var tour: TourProgress?

    #if os(macOS)
    // Match the help button to the navigation height, including its outer padding.
    private let navigationItemHeight: CGFloat = 32
    private let navigationItemWidth: CGFloat = 74
    private let navigationIconSize: CGFloat = 18
    private let navigationFont: Font = .system(size: 14)
    private let helpItemSize: CGFloat = 36
    private let helpIconSize: CGFloat = 18
    #else
    private let navigationItemHeight: CGFloat = 32
    private let navigationItemWidth: CGFloat = 68
    private let navigationIconSize: CGFloat = 16
    private let navigationFont: Font = AppFont.callout
    private let helpItemSize: CGFloat = 32
    private let helpIconSize: CGFloat = 18
    #endif

    var body: some View {
        VStack(spacing: 0) {
            #if !os(macOS)
            HStack(spacing: 12) {
                HStack(spacing: 8) {
                    Image("AppIconDisplay").resizable().frame(width: 28, height: 28).accessibilityHidden(true)
                    Text(SharedCopy.t("app.name")).font(AppFont.headline)
                }
                Spacer(minLength: 8)
                navigation
                Spacer(minLength: 8)
                helpButton
            }
            .padding(.horizontal, 20)
            .frame(height: SharedTokens.sizeTopBarHeight)
            .background(.bar)
            Divider()
            #endif
            if let progress = tour {
                OnboardingView(progress: progress, back: { advanceTour(-1) }, next: { advanceTour(1) },
                               refocus: focusTour, close: { finishTour(.skipped) })
            }
            if model.isPreview {
                Text("离线预览 · 不联网，不读取账号")
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                    .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 24).padding(.top, 8)
            }
            if showWhatsNew {
                HStack(spacing: 12) {
                    Text(SharedCopy.t("dashboard.whatsNew")).font(AppFont.caption)
                    Spacer()
                    Button(SharedCopy.t("action.rewatchTutorial")) { showWhatsNew = false; TutorialState.dismissWhatsNew(.standard); route(.tutorial) }
                    Button(SharedCopy.t("tutorial.whatsNew.dismiss")) { showWhatsNew = false; TutorialState.dismissWhatsNew(.standard) }
                }.padding(.horizontal, 24).padding(.vertical, 10).background(Color.token("accentSurface"))
            }
            VStack(spacing: 0) {
                switch page {
                case .connection: ConnectionPage(open: route, focusRequest: $focusRequest)
                case .records: RecordsPage()
                case .settings: SettingsPage(open: route, focusRequest: $focusRequest, appearance: appearanceSelection)
                }
            }
            .overlayPreferenceValue(TourAnchorPreference.self) { anchors in
                TourSpotlight(target: tour?.step.target.flatMap { anchors[$0] })
            }
        }
        .foregroundStyle(Color.token("textPrimary"))
        .background(Color.token("background"))
        .sheet(item: $sheet) { item in
            SheetHost(sheet: item, close: { sheet = nil })
                .environmentObject(model)
                .appAppearance(AppearancePreference(rawValue: appearanceSelection.wrappedValue) ?? .system)
        }
        .appAppearance(AppearancePreference(rawValue: appearanceSelection.wrappedValue) ?? .system)
        .tint(Color.token("accent"))
        .alert(SharedCopy.t("app.name"), isPresented: Binding(get: { model.errorMessage != nil }, set: { if !$0 { model.errorMessage = nil } })) {
            Button(SharedCopy.t("action.close"), role: .cancel) { model.errorMessage = nil }
        } message: { Text(model.errorMessage ?? "") }
        .task {
            if model.sessionReadOnly { previewAppearance = appearance }
            if TutorialState.shouldPresentAutomatically(defaults: .standard, legacy: model.legacyUserAtLaunch) && !model.isPreview && !model.sessionReadOnly { startTour() }
            showWhatsNew = !model.isPreview && !model.sessionReadOnly && TutorialState.shouldShowWhatsNew(defaults: .standard, legacy: model.legacyUserAtLaunch)
        }
        #if os(macOS)
        .background(DashboardToolbarBridge(toolbarContent: AnyView(
            dashboardToolbar.environment(\.colorScheme, colorScheme))))
        #endif
        .background {
            HStack {
                Button("") { page = .connection }.keyboardShortcut("1", modifiers: .command)
                Button("") { page = .records }.keyboardShortcut("2", modifiers: .command)
                Button("") { page = .settings }.keyboardShortcut("3", modifiers: .command)
                Button("") { model.checkConnection() }.keyboardShortcut("r", modifiers: .command)
            }.frame(width: 0, height: 0).opacity(0).allowsHitTesting(false).accessibilityHidden(true)
        }
    }

    #if os(macOS)
    private var dashboardToolbar: some View {
        HStack(spacing: 8) {
            Text(SharedCopy.t("app.name")).font(AppFont.headline).fixedSize()
            Spacer(minLength: 8)
            navigation
            Spacer(minLength: 8)
            helpButton
        }
        .padding(.trailing, 4)
        .frame(minWidth: DashboardLayout.toolbarMinimumWidth, maxWidth: .infinity)
        .frame(height: DashboardLayout.toolbarHeight)
        .foregroundStyle(Color.token("textPrimary"))
        .tint(Color.token("accent"))
        .transaction { transaction in
            transaction.animation = nil
            transaction.disablesAnimations = true
        }
    }
    #endif

    private var helpButton: some View {
        Button { route(.help) } label: {
            AppIcon(name: "help", size: helpIconSize, color: .token("textSecondary"))
                .frame(width: helpItemSize, height: helpItemSize).contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(SharedCopy.t("action.help"))
        .accessibilityIdentifier("navigation.help")
    }

    private var navigation: some View {
        HStack(spacing: 2) {
            ForEach(AppPage.allCases) { item in
                Button { page = item } label: {
                    HStack(spacing: 6) {
                        AppIcon(name: item.icon, size: navigationIconSize, color: .token(page == item ? "accent" : "textSecondary"))
                        Text(item.title).font(navigationFont.weight(page == item ? .semibold : .regular))
                    }
                    .fixedSize(horizontal: true, vertical: false)
                    .frame(minWidth: navigationItemWidth, minHeight: navigationItemHeight)
                    .foregroundStyle(Color.token(page == item ? "accent" : "textSecondary"))
                    .background(page == item ? Color.token("surface") : .clear, in: RoundedRectangle(cornerRadius: 8))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text(item.title))
                .accessibilityIdentifier("navigation." + item.rawValue)
                .accessibilityValue(page == item ? "已选择" : "")
            }
        }
        .padding(2).background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: 10))
    }

    private func route(_ target: DashboardDestination) {
        switch target {
        case .account, .network:
            sheet = nil
            page = .connection
            focusRequest = DashboardFocusRequest(section: target.rawValue)
        case .tutorial: startTour()
        case .wifiRecovery:
            sheet = nil
            page = .settings
            focusRequest = DashboardFocusRequest(section: "wifiRecovery")
        case .help: sheet = .help
        case .about: sheet = .about
        }
    }

    private var appearanceSelection: Binding<String> {
        model.isPreview || model.sessionReadOnly ? $previewAppearance : $appearance
    }
    private var canSaveTourState: Bool { !model.isPreview && !model.sessionReadOnly }
    private func startTour() {
        sheet = nil
        showWhatsNew = false
        tour = TourProgress()
        focusTour()
    }
    private func advanceTour(_ direction: Int) {
        guard var progress = tour else { return }
        if direction > 0 && progress.isLast { finishTour(.completed); return }
        progress.move(by: direction)
        tour = progress
        focusTour()
    }
    private func focusTour() {
        guard let step = tour?.step else { return }
        page = step.usesSettings ? .settings : .connection
        focusRequest = DashboardFocusRequest(section: step.section)
    }
    private func finishTour(_ status: TutorialState.Status) {
        if canSaveTourState {
            if status == .completed || TutorialState.status(.standard) == .none {
                TutorialState.setStatus(status, .standard)
            }
            TutorialState.dismissWhatsNew(.standard)
        }
        tour = nil
    }
}

struct SheetHost: View {
    let sheet: AppSheet
    let close: () -> Void
    var body: some View {
            #if os(macOS)
            VStack(spacing: 0) {
                HStack(spacing: SharedTokens.spaceM) {
                    AppIcon(name: sheet == .help ? "help" : "info", size: 20, color: .token("accent"))
                        .frame(width: 32, height: 32)
                        .background(Color.token("accentSurface"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium))
                    Text(title).font(AppFont.title).accessibilityAddTraits(.isHeader)
                    Spacer(minLength: SharedTokens.spaceS)
                    Button(action: close) {
                        AppIcon(name: "x", size: 16, color: .token("textSecondary"))
                            .frame(width: SharedTokens.sizeMinTarget, height: SharedTokens.sizeMinTarget)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .keyboardShortcut(.cancelAction)
                    .accessibilityLabel(SharedCopy.t("action.close"))
                    .accessibilityIdentifier("sheet.close")
                    .help(SharedCopy.t("action.close"))
                }
                .padding(.horizontal, SharedTokens.spaceXl)
                .padding(.vertical, SharedTokens.spaceS)
                .background(Color.token("surface"))
                Divider()
                sheetContent
            }
            .foregroundStyle(Color.token("textPrimary"))
            .background(Color.token("background"))
            .frame(minWidth: 480, idealWidth: 520, minHeight: 440, idealHeight: 620)
            #else
            NavigationStack {
                sheetContent
                    .navigationTitle(title)
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button(SharedCopy.t("action.close"), action: close).keyboardShortcut(.cancelAction)
                        }
                    }
            }
            #endif
    }

    private var title: String { SharedCopy.t(sheet == .help ? "help.title" : "about.title") }

    @ViewBuilder private var sheetContent: some View {
        switch sheet {
        case .help: HelpSheet()
        case .about: AboutSheet()
        }
    }
}
