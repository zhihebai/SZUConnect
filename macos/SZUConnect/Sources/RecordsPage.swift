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
    @State private var query = ""
    @State private var showingSamples = false
    @State private var failuresOnly = false
    @State private var exporting = false
    @State private var exportDocument: DiagnosticsDocument?
    @State private var feedback: String?
    @State private var sampleSelection = RecordSampleSelection()

    private static let failureMarkers = ["登录未完成", "手动检测未通过", "拒绝了登录", "外网检测暂未通过", "网络路径暂不可用", "认证服务暂时连不上", "登录被拒绝", "网络连接暂不可用", "校园网登录服务暂时连不上", "登录未通过"]
    private func isFailure(_ event: ConnectionEvent) -> Bool {
        RecordMessageClassifier.isFailure(event.message, legacyMarkers: Self.failureMarkers)
    }

    private var shown: [ConnectionEvent] {
        model.events.reversed().filter { event in
            (!failuresOnly || isFailure(event)) && (query.isEmpty || event.message.localizedCaseInsensitiveContains(query))
        }
    }

    var body: some View {
        PageScroll(title: SharedCopy.t("records.title")) {
            health
            Picker(SharedCopy.t("records.title"), selection: $showingSamples) {
                Text(SharedCopy.t("records.activities")).tag(false)
                Text(SharedCopy.t("records.samples")).tag(true)
            }.pickerStyle(.segmented)
            TextField(SharedCopy.t("records.search"), text: $query)
                .textFieldStyle(.roundedBorder)
                .frame(minHeight: SharedTokens.sizeMinTarget)
            Toggle(SharedCopy.t("records.failuresOnly"), isOn: $failuresOnly).frame(minHeight: SharedTokens.sizeMinTarget)
            HStack(spacing: SharedTokens.spaceS) {
                ActionButton(title: SharedCopy.t("action.exportDiagnostics"), icon: "download") {
                    exportDocument = DiagnosticsDocument(data: model.diagnosticExportData)
                    exporting = true
                }
                ActionButton(title: SharedCopy.t("action.copyDiagnostics"), icon: "copy") {
                    Clipboard.copy(model.diagnosticSummary)
                    feedback = SharedCopy.t("records.copyDone")
                }
            }
            if let feedback { Text(feedback).font(AppFont.caption).foregroundStyle(Color.tone("success")) }
            if showingSamples { sampleList } else { list }
        }
        .fileExporter(isPresented: $exporting, document: exportDocument, contentType: .json,
                      defaultFilename: "SZUConnect-diagnostics") { result in
            defer { exportDocument = nil }
            switch result {
            case .success: feedback = SharedCopy.t("records.exportDone")
            case .failure(let error):
                let value = error as NSError
                if value.domain != NSCocoaErrorDomain || value.code != NSUserCancelledError {
                    feedback = SharedCopy.t("records.exportFailed")
                }
            }
        }
    }

    private static func stamp(_ date: Date) -> String {
        date.formatted(.dateTime.month().day().hour().minute().second())
    }

    private var health: some View {
        let samples = model.currentHealth.samples
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
        let items = model.currentHealth.samples.reversed().filter { sample in
            let text = [sample.resultTitle, sample.diagnosis.title, sample.interface,
                        endpointTitle(sample.appleState), endpointTitle(sample.microsoftState)].joined(separator: " ")
            return (!failuresOnly || !sample.online || sample.isPartialCompleteResult)
                && (query.isEmpty || text.localizedCaseInsensitiveContains(query))
        }
        if items.isEmpty {
            Card(fill: .token("surfaceMuted")) {
                Text(SharedCopy.t(model.currentHealth.samples.isEmpty ? "records.empty.title" : "records.noMatch.title")).font(AppFont.headline)
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
            let none = model.events.isEmpty
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
