import SwiftUI

struct OpenSourceLicense: Identifiable, Sendable {
    let id: String
    let title: String
    let text: String?
}

enum OpenSourceLicenses {
    // The signed main bundle is immutable for the life of the process. A new
    // SwiftUI view value must not synchronously reread these files on every update.
    private static let bundledDocuments = readDocuments(in: .main)

    static func documents(in bundle: Bundle = .main) -> [OpenSourceLicense] {
        if bundle === Bundle.main { return bundledDocuments }
        // Explicit bundles are used by previews/tests; keep their reads fresh.
        return readDocuments(in: bundle)
    }

    private static func readDocuments(in bundle: Bundle) -> [OpenSourceLicense] {
        var resources: [(id: String, title: String, name: String, suffix: String?)] = [
            ("project", "SZUConnect · MIT", "LICENSE", nil),
            ("autoszuweb", "AutoSZUWeb · MIT", "AutoSZUWeb-LICENSE", "txt"),
        ]
        #if os(macOS)
        resources.append(("curl", "libcurl · curl", "curl-COPYING", "txt"))
        #endif
        return resources.map { resource in
            let text = bundle.url(forResource: resource.name, withExtension: resource.suffix)
                .flatMap { try? String(contentsOf: $0, encoding: .utf8) }
                .flatMap { $0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : $0 }
            return OpenSourceLicense(id: resource.id, title: resource.title, text: text)
        }
    }
}

/// All text comes from bundled license files; opening or copying never makes a request.
struct OpenSourceLicensesView: View {
    private let licenses = OpenSourceLicenses.documents()
    @State private var copiedLicense: String?
    @State private var disclosure = LicenseDisclosureState()

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            DisclosureHeader(isExpanded: $disclosure.groupExpanded) {
                AppIcon(name: "book", size: 20, color: .token("accent"))
                Text(SharedCopy.t("about.licenses.title") + " · " + SharedCopy.f("about.licenses.count", ["count": String(licenses.count)]))
                    .font(AppFont.headline).accessibilityAddTraits(.isHeader)
            }.accessibilityIdentifier("about.licenses")
            if disclosure.groupExpanded { licenseDocuments }
        }
        .foregroundStyle(Color.token("textPrimary"))
    }

    private var licenseDocuments: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
            Text(SharedCopy.t("about.licenses.detail"))
                .font(AppFont.callout)
                .foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
            ForEach(licenses) { license in
                Divider()
                DisclosureGroup(isExpanded: Binding(
                    get: { disclosure.isExpanded(license.id) },
                    set: { disclosure.setExpanded(license.id, $0) })) {
                    VStack(alignment: .leading, spacing: SharedTokens.spaceM) {
                        if let text = license.text {
                            Button {
                                Clipboard.copy(text)
                                copiedLicense = license.id
                            } label: {
                                Label {
                                    Text(SharedCopy.t(copiedLicense == license.id ? "action.copied" : "action.copy"))
                                } icon: {
                                    AppIcon(name: copiedLicense == license.id ? "check" : "copy", size: 14)
                                }
                            }
                            .buttonStyle(.bordered)
                            .accessibilityLabel(license.title + "，" + SharedCopy.t(copiedLicense == license.id ? "action.copied" : "action.copy"))
                            .accessibilityIdentifier("about.license.copy." + license.id)
                            Text(text)
                                .font(AppFont.callout)
                                .textSelection(.enabled)
                                .fixedSize(horizontal: false, vertical: true)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .accessibilityIdentifier("about.license.text." + license.id)
                        } else {
                            Text(SharedCopy.t("about.licenses.unavailable"))
                                .font(AppFont.callout)
                                .foregroundStyle(Color.token("warning"))
                        }
                    }
                    .padding(.bottom, SharedTokens.spaceS)
                } label: {
                    Text(license.title).font(AppFont.bodyStrong)
                        .accessibilityIdentifier("about.license." + license.id)
                }
                .disclosureGroupStyle(FullRowDisclosureStyle())
            }
        }
        .foregroundStyle(Color.token("textPrimary"))
    }
}
