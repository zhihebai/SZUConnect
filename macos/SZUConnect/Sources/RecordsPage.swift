import Foundation

/// Selection is local to the visible ribbon; dates need not be unique.
struct RecordSampleSelection {
    private(set) var id: UUID?

    mutating func toggle(_ sampleID: UUID) {
        id = id == sampleID ? nil : sampleID
    }

    mutating func retain(among visibleIDs: [UUID]) {
        if let id, !visibleIDs.contains(id) { self.id = nil }
    }

    mutating func clear() { id = nil }

    func selected<Sample: Identifiable>(from samples: [Sample]) -> Sample? where Sample.ID == UUID {
        samples.first { $0.id == id }
    }
}

import SwiftUI
import UniformTypeIdentifiers

struct DiagnosticsDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.json] }
    var data: Data
    init(data: Data) { self.data = data }
    init(configuration: ReadConfiguration) throws { data = configuration.file.regularFileContents ?? Data() }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper { FileWrapper(regularFileWithContents: data) }
}

struct RecordsPage: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        RecordsContent(events: model.events, samples: model.currentHealth.samples,
            sourceID: ObjectIdentifier(model),
            canCollectCrashMetadata: model.canCollectCrashMetadata,
            diagnosticData: { model.diagnosticExportData },
            diagnosticSummary: { model.diagnosticSummary })
            .equatable()
    }
}

/// Progress updates do not change history. Keep filtering, statistics and local
/// presentation state behind the value inputs that actually affect this page.
struct RecordsContent: View, Equatable {
    let events: [ConnectionEvent]
    let samples: [ConnectionSample]
    let sourceID: ObjectIdentifier
    let canCollectCrashMetadata: Bool
    let diagnosticData: () -> Data
    let diagnosticSummary: () -> String

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.sourceID == rhs.sourceID && lhs.events == rhs.events && lhs.samples == rhs.samples
            && lhs.canCollectCrashMetadata == rhs.canCollectCrashMetadata
    }

    @State private var query = ""
    @State private var showingSamples = false
    @State private var failuresOnly = false
    @State private var exporting = false
    @State private var exportDocument: DiagnosticsDocument?
    @State private var feedback: String?
    @State private var feedbackIsFailure = false
    @State private var sampleSelection = RecordSampleSelection()
    @StateObject private var reportExport = DiagnosticReportExportCoordinator()
    #if os(macOS)
    @State private var savePanel: NSSavePanel?
    #endif

    private static let failureMarkers = ["登录未完成", "手动检测未通过", "拒绝了登录", "外网检测暂未通过", "网络路径暂不可用", "认证服务暂时连不上", "登录被拒绝", "网络连接暂不可用", "校园网登录服务暂时连不上", "登录未通过"]
    private func isFailure(_ event: ConnectionEvent) -> Bool {
        RecordMessageClassifier.isFailure(event, legacyMarkers: Self.failureMarkers)
    }

    private var shown: [ConnectionEvent] {
        events.reversed().filter { event in
            (!failuresOnly || isFailure(event)) && (query.isEmpty || event.message.localizedCaseInsensitiveContains(query))
        }
    }

    var body: some View {
        PageScroll(title: SharedCopy.t("records.title")) {
            health
            diagnostics
            Picker(SharedCopy.t("records.title"), selection: $showingSamples) {
                Text(SharedCopy.t("records.activities")).tag(false)
                Text(SharedCopy.t("records.samples")).tag(true)
            }.pickerStyle(.segmented)
            TextField(SharedCopy.t("records.search"), text: $query)
                .textFieldStyle(.roundedBorder)
                .frame(minHeight: SharedTokens.sizeMinTarget)
            Toggle(SharedCopy.t("records.failuresOnly"), isOn: $failuresOnly).frame(minHeight: SharedTokens.sizeMinTarget)
            if showingSamples { sampleList } else { list }
        }
        #if os(macOS)
        .background(ReportExportWindowObserver(onClose: cancelExport).frame(width: 0, height: 0))
        #endif
        .onDisappear { cancelExport() }
        .onChange(of: reportExport.state) { _, value in
            switch value {
            case .saved: feedback = SharedCopy.t("records.exportDone"); feedbackIsFailure = false
            case .cancelled: feedback = SharedCopy.t("records.exportCancelled"); feedbackIsFailure = false
            case .failed: feedback = SharedCopy.t("records.exportFailed"); feedbackIsFailure = true
            default: break
            }
        }
        .fileExporter(isPresented: $exporting, document: exportDocument, contentType: .json,
                      defaultFilename: "SZUConnect-diagnostics") { result in
            defer { exportDocument = nil }
            switch result {
            case .success: feedback = SharedCopy.t("records.exportDone"); feedbackIsFailure = false
            case .failure(let error):
                let value = error as NSError
                if value.domain != NSCocoaErrorDomain || value.code != NSUserCancelledError {
                    feedback = SharedCopy.t("records.exportFailed")
                    feedbackIsFailure = true
                }
            }
        }
    }

    private var diagnostics: some View {
        Card {
            HStack(spacing: SharedTokens.spaceXs) {
                Text(SharedCopy.t("records.diagnostics.title")).font(AppFont.headline).accessibilityAddTraits(.isHeader)
                InfoButton(topic: "recordDiagnostics")
                Spacer(minLength: 0)
            }
            Text(SharedCopy.t("records.diagnostics.copyHint")).font(AppFont.callout)
                .foregroundStyle(Color.token("textSecondary")).fixedSize(horizontal: false, vertical: true)
            ActionButton(title: SharedCopy.t("action.copyDiagnostics"), icon: "copy", action: copySummary)
                .accessibilityIdentifier("records.copySummary")
            Divider().padding(.vertical, SharedTokens.spaceXs)
            Text(SharedCopy.t("records.diagnostics.exportHint")).font(AppFont.callout)
                .foregroundStyle(Color.token("textSecondary")).fixedSize(horizontal: false, vertical: true)
            HStack(spacing: SharedTokens.spaceS) {
                ActionButton(title: SharedCopy.t("action.exportDiagnostics"), icon: "download", action: chooseReportDestination)
                    .disabled(reportExport.busy || exporting).accessibilityIdentifier("records.exportReport")
                if reportExport.state == .preparing || reportExport.state == .stopping {
                    ProgressView().controlSize(.small)
                    ActionButton(title: SharedCopy.t("records.cancelExport"), action: cancelExport)
                        .disabled(reportExport.state == .stopping).accessibilityIdentifier("records.cancelExport")
                }
            }
            if reportExport.state == .preparing || reportExport.state == .stopping {
                Text(SharedCopy.t(reportExport.state == .stopping ? "records.exportCancelling" : "records.exporting"))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            } else if let feedback {
                Text(feedback).font(AppFont.caption).foregroundStyle(Color.tone(feedbackIsFailure ? "warning" : "success"))
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func copySummary() {
        // Capture the current model on click. Search and list filters are local
        // presentation state and never reduce this snapshot's scope.
        let summary = diagnosticSummary()
        #if os(macOS)
        NSPasteboard.general.clearContents()
        let copied = NSPasteboard.general.setString(summary, forType: .string)
        feedback = SharedCopy.t(copied ? "records.copyDone" : "records.copyFailed")
        feedbackIsFailure = !copied
        #else
        Clipboard.copy(summary)
        feedback = SharedCopy.t("records.copyDone"); feedbackIsFailure = false
        #endif
    }

    private func chooseReportDestination() {
        #if os(macOS)
        guard reportExport.beginChoosing() else { return }
        feedback = nil
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "SZUConnect-diagnostics.json"
        panel.canCreateDirectories = true
        savePanel = panel
        let completion: (NSApplication.ModalResponse) -> Void = { response in
            savePanel = nil
            guard response == .OK, let destination = panel.url, reportExport.state == .choosing else {
                reportExport.selectionCancelled(); return
            }
            // Do not freeze or query any source until the user chooses a file.
            // The immutable Data is the sole app snapshot for the whole export.
            let snapshot = diagnosticData()
            let identity = DiagnosticCrashIdentity(bundleIdentifier: Bundle.main.bundleIdentifier ?? "",
                version: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "",
                build: Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "",
                executablePath: Bundle.main.executableURL?.standardizedFileURL.path ?? "")
            reportExport.start(snapshot: snapshot, destination: destination, identity: identity,
                               allowCrashCollection: canCollectCrashMetadata)
        }
        if let window = NSApp.keyWindow { panel.beginSheetModal(for: window, completionHandler: completion) }
        else { panel.begin(completionHandler: completion) }
        #else
        exportDocument = DiagnosticsDocument(data: diagnosticData())
        exporting = true
        #endif
    }

    private func cancelExport() {
        #if os(macOS)
        savePanel?.cancel(nil)
        savePanel = nil
        #endif
        reportExport.cancel()
    }

    private static func stamp(_ date: Date) -> String {
        date.formatted(.dateTime.month().day().hour().minute().second())
    }

    private var health: some View {
        let online = samples.filter(\.online).count
        let latencies = samples.compactMap(\.latencyMilliseconds).sorted()
        let recent = Array(samples.suffix(24))
        return Card {
            HStack(spacing: 4) {
                Text(SharedCopy.t("records.health.title")).font(AppFont.headline).accessibilityAddTraits(.isHeader)
                InfoButton(topic: "records")
                Spacer(minLength: 0)
            }
            if samples.isEmpty {
                Text(SharedCopy.t("records.range.none")).font(AppFont.body).foregroundStyle(Color.token("textSecondary"))
            } else {
                Text(Self.healthLine(count: samples.count, online: online, latencies: latencies)).font(AppFont.bodyStrong)
            }
            // Show retention, the stored time range and the 24 newest samples.
            VStack(alignment: .leading, spacing: 2) {
                Text(SharedCopy.f("records.range.retention", ["max": "360"]))
                if let first = samples.first?.date, let last = samples.last?.date {
                    Text(SharedCopy.f("records.range.stored", ["count": String(samples.count), "from": Self.stamp(first), "to": Self.stamp(last)]))
                }
                if let ribbonFirst = recent.first?.date, let ribbonLast = recent.last?.date {
                    Text(SharedCopy.f("records.range.ribbon", ["count": String(recent.count), "from": Self.stamp(ribbonFirst), "to": Self.stamp(ribbonLast)]))
                }
            }
            .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            if !recent.isEmpty {
                HStack(spacing: 3) {
                    ForEach(recent) { sample in
                        Button { sampleSelection.toggle(sample.id) } label: {
                            RoundedRectangle(cornerRadius: 3)
                                .fill(Color.tone(sample.online && !sample.isPartialCompleteResult ? "success" : "warning"))
                                .frame(height: 22)
                                .overlay(RoundedRectangle(cornerRadius: 3)
                                    .strokeBorder(Color.token("textPrimary"), lineWidth: sampleSelection.id == sample.id ? 2 : 0))
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .help(sample.date.formatted(date: .abbreviated, time: .standard) + " · " + sample.resultTitle)
                        .accessibilityLabel(sample.date.formatted(date: .abbreviated, time: .standard) + " · " + sample.resultTitle)
                    }
                }
                Text(SharedCopy.t("records.ribbon.hint")).font(AppFont.caption).foregroundStyle(Color.token("textTertiary"))
                if let picked = sampleSelection.selected(from: recent) { detail(picked) }
            }
            let complete = samples.filter { $0.probeMode == "complete" }
            if !complete.isEmpty {
                Text(SharedCopy.f("dashboard.completeHistory", ["passed": String(complete.filter(\.allSelectedPassed).count), "total": String(complete.count)]))
                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
            }
        }
        .onChange(of: recent.map(\.id), initial: true) { _, visibleIDs in
            sampleSelection.retain(among: visibleIDs)
        }
    }

    private func detail(_ sample: ConnectionSample) -> some View {
        let network = NetworkNaming.human(sample.interface)
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(SharedCopy.t("records.detail.title")).font(AppFont.bodyStrong)
                Spacer(minLength: 0)
                Button(SharedCopy.t("records.detail.close")) { sampleSelection.clear() }
            }
            detailRow("records.detail.time", sample.date.formatted(date: .abbreviated, time: .standard))
            detailRow("records.detail.mode", SharedCopy.t(sample.probeMode == "complete" ? "probe.complete.title" : "probe.quick.title"))
            detailRow("records.detail.result", sample.resultTitle)
            detailRow("records.detail.apple", endpointTitle(sample.appleState))
            detailRow("records.detail.microsoft", endpointTitle(sample.microsoftState))
            if let count = sample.websiteTotal, count > 0 {
                detailRow("records.detail.sites", String(sample.websitePassed ?? 0) + " / " + String(count))
                if let passed = sample.websitePassed, passed < count {
                    Text(SharedCopy.t("records.websiteWarning"))
                        .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if let latency = sample.latencyMilliseconds {
                detailRow("records.detail.latency", String(Int(latency.rounded())) + " ms")
            }
            detailRow("records.detail.link", network.title)
            if !network.technical.isEmpty { detailRow("records.detail.technical", network.technical) }
        }
        .padding(SharedTokens.spaceM)
        .background(Color.token("surfaceMuted"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium, style: .continuous))
    }

    private func detailRow(_ key: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(SharedCopy.t(key)).font(AppFont.caption).foregroundStyle(Color.token("textSecondary")).frame(width: 72, alignment: .leading)
            Text(value).font(AppFont.callout).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
    }

    private static func healthLine(count: Int, online: Int, latencies: [Double]) -> String {
        let percent = (online * 100 + count / 2) / max(count, 1)
        var line = SharedCopy.f("records.health.samples", ["count": String(count)]) + " · "
            + SharedCopy.f("records.health.successRate", ["percent": String(percent)])
        if !latencies.isEmpty {
            line += " · " + SharedCopy.f("records.health.latency", ["ms": String(Int((latencies.count % 2 == 0 ? (latencies[latencies.count / 2 - 1] + latencies[latencies.count / 2]) / 2 : latencies[latencies.count / 2]).rounded()))])
        }
        return line
    }

    @ViewBuilder private var sampleList: some View {
        let items = samples.reversed().filter { sample in
            let text = [sample.resultTitle, sample.diagnosis.title, sample.interface,
                        endpointTitle(sample.appleState), endpointTitle(sample.microsoftState)].joined(separator: " ")
            return (!failuresOnly || sample.isHistoricalFailure)
                && (query.isEmpty || text.localizedCaseInsensitiveContains(query))
        }
        if items.isEmpty {
            Card(fill: .token("surfaceMuted")) {
                Text(SharedCopy.t(samples.isEmpty ? "records.empty.title" : "records.noMatch.title")).font(AppFont.headline)
                Text(SharedCopy.t("records.noMatch.detail")).font(AppFont.caption)
            }
        } else {
            Card(padding: SharedTokens.spaceS) {
                LazyVStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                    ForEach(items) { sample in
                        VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                            Text(sample.date.formatted(date: .abbreviated, time: .standard)).font(AppFont.caption).foregroundStyle(Color.token("textTertiary"))
                            Text(sample.resultTitle).font(AppFont.bodyStrong)
                                .foregroundStyle(Color.tone(sample.online && !sample.isPartialCompleteResult ? "success" : "warning"))
                            Text("Apple：" + endpointTitle(sample.appleState) + " · Microsoft：" + endpointTitle(sample.microsoftState))
                                .font(AppFont.caption).fixedSize(horizontal: false, vertical: true)
                            if let count = sample.websiteTotal, count > 0 {
                                Text(SharedCopy.f("dashboard.siteHistory", ["passed": String(sample.websitePassed ?? 0), "total": String(count)]))
                                    .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                            }
                            Text(NetworkNaming.human(sample.interface).title + (sample.latencyMilliseconds.map { " · " + String(Int($0.rounded())) + " ms" } ?? ""))
                                .font(AppFont.caption).foregroundStyle(Color.token("textSecondary"))
                        }.padding(SharedTokens.spaceS).accessibilityElement(children: .combine)
                        Divider()
                    }
                }
            }
        }
    }
    private func endpointTitle(_ value: String) -> String {
        ConnectivityEndpointState(rawValue: value)?.title ?? SharedCopy.t("env.access.unknown")
    }

    @ViewBuilder private var list: some View {
        let items = shown
        if items.isEmpty {
            let none = events.isEmpty
            Card(fill: .token("surfaceMuted")) {
                HStack(spacing: SharedTokens.spaceS) {
                    AppIcon(name: none ? "records" : "search", size: 22, color: .token("textTertiary"))
                    Text(SharedCopy.t(none ? "records.empty.title" : "records.noMatch.title")).font(AppFont.headline)
                }
                Text(SharedCopy.t(none ? "records.empty.detail" : "records.noMatch.detail")).font(AppFont.body)
                    .foregroundStyle(Color.token("textSecondary"))
            }
        } else {
            Card(padding: SharedTokens.spaceS) {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(items) { event in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(event.date.formatted(date: .abbreviated, time: .shortened))
                                .font(AppFont.caption).foregroundStyle(isFailure(event) ? Color.tone("warning") : Color.token("textTertiary"))
                            Text(event.message).font(AppFont.body).foregroundStyle(Color.token("textPrimary"))
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .padding(.vertical, SharedTokens.spaceS)
                        .padding(.horizontal, SharedTokens.spaceM)
                        .accessibilityElement(children: .combine)
                        Divider()
                    }
                }
            }
        }
    }
}

#if os(macOS)
/// A retained NSHostingView does not receive onDisappear when its NSWindow is
/// closed. Observe only the containing window, without changing its delegate.
private struct ReportExportWindowObserver: NSViewRepresentable {
    var onClose: () -> Void
    func makeNSView(context: Context) -> ObserverView { ObserverView(onClose: onClose) }
    func updateNSView(_ nsView: ObserverView, context: Context) { nsView.onClose = onClose }
    final class ObserverView: NSView {
        var onClose: () -> Void
        private var observation: NSObjectProtocol?
        init(onClose: @escaping () -> Void) { self.onClose = onClose; super.init(frame: .zero) }
        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let observation { NotificationCenter.default.removeObserver(observation); self.observation = nil }
            guard let window else { return }
            observation = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification,
                object: window, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.onClose() }
                }
        }
        deinit { if let observation { NotificationCenter.default.removeObserver(observation) } }
    }
}
#endif


/// Human names for interfaces; technical identifiers (en0, en5) only appear in the detail view.
enum NetworkNaming {
    static func human(_ raw: String) -> (title: String, technical: String) {
        let parts = raw.components(separatedBy: " · ").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        guard !parts.isEmpty else { return (SharedCopy.t("env.access.unknown"), "") }
        var names: [String] = []
        var identifiers: [String] = []
        for part in parts {
            let words = part.split(separator: " ").map(String.init)
            let name = part.hasPrefix("Wi-Fi") ? "Wi-Fi" : part.hasPrefix("有线") ? SharedCopy.t("env.access.ethernet") : (words.first ?? part)
            if !names.contains(name) { names.append(name) }
            if words.count > 1, let identifier = words.last { identifiers.append(identifier) }
        }
        return (names.joined(separator: " · "), identifiers.joined(separator: ", "))
    }
}
