import SwiftUI
#if os(macOS)
import AppKit
#else
import UIKit
#endif

// Design tokens and icons are generated from shared/.

enum AppearancePreference: String, CaseIterable, Identifiable {
    case system, light, dark
    var id: String { rawValue }
    var colorScheme: ColorScheme? { self == .light ? .light : self == .dark ? .dark : nil }
    var title: String { SharedCopy.t("settings.appearance.\(rawValue)") }
}

extension View {
    @ViewBuilder
    func appAppearance(_ preference: AppearancePreference) -> some View {
        #if os(macOS)
        modifier(AppAppearanceModifier(preference: preference))
        #else
        preferredColorScheme(preference.colorScheme)
        #endif
    }
}

#if os(macOS)
private struct AppAppearanceModifier: ViewModifier {
    let preference: AppearancePreference
    @StateObject private var applicationAppearance = ApplicationAppearanceObserver()

    func body(content: Content) -> some View {
        // Native alerts inherit the application appearance, outside SwiftUI's presentation.
        // A concrete effective scheme also refreshes existing sheets when system mode changes.
        content.preferredColorScheme(preference.colorScheme ?? applicationAppearance.colorScheme)
            .onChange(of: preference, initial: true) { _, value in
                applicationAppearance.apply(value)
            }
    }
}

@MainActor
private final class ApplicationAppearanceObserver: ObservableObject {
    @Published private(set) var colorScheme: ColorScheme
    private var observation: NSKeyValueObservation?

    init() {
        let application = NSApplication.shared
        colorScheme = Self.resolve(application.effectiveAppearance)
        observation = application.observe(\.effectiveAppearance, options: [.new]) { [weak self] _, _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                let scheme = Self.resolve(NSApplication.shared.effectiveAppearance)
                if colorScheme != scheme { colorScheme = scheme }
            }
        }
    }

    func apply(_ preference: AppearancePreference) {
        let name: NSAppearance.Name?
        switch preference {
        case .system: name = nil
        case .light: name = .aqua
        case .dark: name = .darkAqua
        }
        let application = NSApplication.shared
        if application.appearance?.name != name {
            // Clear the app override to follow macOS again; never change system preferences.
            application.appearance = name.flatMap { NSAppearance(named: $0) }
        }
        let scheme = Self.resolve(application.effectiveAppearance)
        if colorScheme != scheme { colorScheme = scheme }
    }

    private static func resolve(_ appearance: NSAppearance) -> ColorScheme {
        appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? .dark : .light
    }
}
#endif

extension Color {
    static func token(_ name: String) -> Color {
        guard let pair = SharedTokens.colors[name] else { return Color.pink }
        #if os(macOS)
        return Color(nsColor: NSColor(name: nil) { appearance in
            let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            let c = dark ? pair.dark : pair.light
            return NSColor(srgbRed: c.r, green: c.g, blue: c.b, alpha: c.a)
        })
        #else
        return Color(uiColor: UIColor { traits in
            let c = traits.userInterfaceStyle == .dark ? pair.dark : pair.light
            return UIColor(red: c.r, green: c.g, blue: c.b, alpha: c.a)
        })
        #endif
    }
    static func tone(_ tone: String) -> Color { .token(SharedTokens.tones[tone]?.foreground ?? "neutral") }
    static func toneSurface(_ tone: String) -> Color { .token(SharedTokens.tones[tone]?.surface ?? "neutralSurface") }
}

enum AppFont {
    static var hero: Font { .system(size: SharedTokens.fontHeroSize, weight: .semibold) }
    static var title: Font { .system(size: SharedTokens.fontTitleSize, weight: .semibold) }
    static var headline: Font { .system(size: SharedTokens.fontHeadlineSize, weight: .semibold) }
    static var body: Font { .system(size: SharedTokens.fontBodySize) }
    static var bodyStrong: Font { .system(size: SharedTokens.fontBodySize, weight: .semibold) }
    static var callout: Font { .system(size: SharedTokens.fontCalloutSize) }
    static var caption: Font { .system(size: SharedTokens.fontCaptionSize) }
}

enum DashboardLayout {
    // Narrow layouts wrap page controls while preserving the readable three-tab toolbar.
    static let minimumWindowWidth: CGFloat = 500
}

/// Draws a shared icon (absolute M/L/C/Z path on a 24-unit grid) as a round-capped stroke.
struct IconShape: Shape {
    let name: String
    func path(in rect: CGRect) -> Path {
        guard let elements = Self.unitElements[name] else { return Path() }
        var path = Path()
        let scale = min(rect.width, rect.height) / 24
        func point(_ p: CGPoint) -> CGPoint { CGPoint(x: rect.minX + p.x * scale, y: rect.minY + p.y * scale) }
        for element in elements {
            switch element {
            case .move(let p): path.move(to: point(p))
            case .line(let p): path.addLine(to: point(p))
            case .quadCurve(let end, let control): path.addQuadCurve(to: point(end), control: point(control))
            case .curve(let end, let c1, let c2): path.addCurve(to: point(end), control1: point(c1), control2: point(c2))
            case .closeSubpath: path.closeSubpath()
            }
        }
        return path
    }

    // SharedIcons is generated, immutable input. Keep a bounded unit-grid cache so
    // layout/redraw only transforms geometry; it never reparses SVG numbers.
    // Keep numeric elements before SwiftUI Path's internal coordinate conversion,
    // preserving the original precision/order when scaling at a nonzero origin.
    private static let unitElements: [String: [Path.Element]] = SharedIcons.paths.mapValues(parse)

    private static func parse(_ data: String) -> [Path.Element] {
        var elements: [Path.Element] = []
        func point(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: x, y: y) }
        let tokens = data.replacingOccurrences(of: ",", with: " ").split(separator: " ")
        var index = 0
        var command: Character = "M"
        func next() -> Double {
            defer { index += 1 }
            return index < tokens.count ? Double(tokens[index]) ?? 0 : 0
        }
        while index < tokens.count {
            let token = tokens[index]
            if let first = token.first, first.isLetter {
                command = first
                if token.count > 1 {
                    // "M12" style: the number is glued to the command letter.
                    let number = Double(token.dropFirst()) ?? 0
                    index += 1
                    switch command {
                    case "M": elements.append(.move(to: point(number, next())))
                    case "L": elements.append(.line(to: point(number, next())))
                    case "C":
                        let c1 = point(number, next()), c2 = point(next(), next()), end = point(next(), next())
                        elements.append(.curve(to: end, control1: c1, control2: c2))
                    default: break
                    }
                    continue
                }
                index += 1
                if command == "Z" { elements.append(.closeSubpath) }
                continue
            }
            switch command {
            case "M": elements.append(.move(to: point(next(), next())))
            case "L": elements.append(.line(to: point(next(), next())))
            case "C":
                let c1 = point(next(), next()), c2 = point(next(), next()), end = point(next(), next())
                elements.append(.curve(to: end, control1: c1, control2: c2))
            default: index += 1
            }
        }
        return elements
    }
}

struct AppIcon: View {
    let name: String
    var size: CGFloat = SharedTokens.sizeIconMedium
    var color: Color = .token("textSecondary")
    var body: some View {
        IconShape(name: name)
            .stroke(color, style: StrokeStyle(lineWidth: size * 1.75 / 24, lineCap: .round, lineJoin: .round))
            .frame(width: size, height: size)
            .accessibilityHidden(true)
    }
}

struct Card<Content: View>: View {
    var fill: Color = .token("surface")
    var padding: CGFloat = SharedTokens.spaceXl
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceM) { content }
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fill, in: RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous).strokeBorder(Color.token("border")))
    }
}

/// A disclosure header is one keyboard-accessible button, including its label and whitespace.
/// Keep trailing actions outside this button so they never compete with expansion.
struct DisclosureHeader<Label: View>: View {
    @Binding var isExpanded: Bool
    @ViewBuilder var label: Label

    var body: some View {
        Button { isExpanded.toggle() } label: {
            HStack(spacing: SharedTokens.spaceS) {
                AppIcon(name: isExpanded ? "chevron-down" : "chevron-right", size: 14)
                label
                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityValue(SharedCopy.t(isExpanded ? "disclosure.expanded" : "disclosure.collapsed"))
    }
}

struct FullRowDisclosureStyle: DisclosureGroupStyle {
    func makeBody(configuration: Configuration) -> some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
            DisclosureHeader(isExpanded: configuration.$isExpanded) { configuration.label }
            if configuration.isExpanded {
                configuration.content
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

enum AppButtonKind { case primary, secondary, plain }

struct AppButtonStyle: ButtonStyle {
    var kind: AppButtonKind = .secondary
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        let pressed = configuration.isPressed
        let background: Color = {
            switch kind {
            case .primary: return .token(pressed ? "accentPressed" : "accent")
            case .secondary: return .token(pressed ? "surfacePressed" : "surface")
            case .plain: return pressed ? .token("surfacePressed") : .clear
            }
        }()
        let foreground: Color = kind == .primary ? .token("onAccent") : kind == .plain ? .token("accent") : .token("textPrimary")
        return configuration.label
            .font(AppFont.bodyStrong)
            .foregroundStyle(enabled ? foreground : Color.token("textTertiary"))
            .padding(.horizontal, SharedTokens.spaceL)
            .frame(minHeight: SharedTokens.sizeMinTarget)
            .background(enabled || kind != .primary ? background : Color.token("surfacePressed"),
                        in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium, style: .continuous))
            .overlay {
                if kind == .secondary {
                    RoundedRectangle(cornerRadius: SharedTokens.radiusMedium, style: .continuous).strokeBorder(Color.token("borderStrong"))
                }
            }
            .contentShape(RoundedRectangle(cornerRadius: SharedTokens.radiusMedium, style: .continuous))
    }
}

struct ActionButton: View {
    let title: String
    var icon: String? = nil
    var kind: AppButtonKind = .secondary
    var fill = false
    var isDefault = false
    let action: () -> Void
    var body: some View {
        let button = Button(action: action) {
            HStack(spacing: SharedTokens.spaceS) {
                if let icon { AppIcon(name: icon, size: 18, color: kind == .primary ? .token("onAccent") : kind == .plain ? .token("accent") : .token("textPrimary")) }
                Text(title).multilineTextAlignment(.center)
            }
            .frame(maxWidth: fill ? .infinity : nil)
        }
        .buttonStyle(AppButtonStyle(kind: kind))
        if isDefault { button.keyboardShortcut(.defaultAction) } else { button }
    }
}

struct Chip: View {
    let text: String
    let tone: String
    var body: some View {
        Text(text)
            .font(AppFont.caption.weight(.medium))
            .foregroundStyle(Color.tone(tone))
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(Color.toneSurface(tone), in: Capsule())
    }
}

struct Banner: View {
    let tone: String
    let title: String
    var detail: String? = nil
    var actionTitle: String? = nil
    var action: (() -> Void)? = nil
    var body: some View {
        HStack(alignment: .top, spacing: SharedTokens.spaceM) {
            AppIcon(name: SharedTokens.tones[tone]?.icon ?? "info", size: 20, color: .tone(tone))
            VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
                Text(title).font(AppFont.bodyStrong).foregroundStyle(Color.token("textPrimary"))
                if let detail { Text(detail).font(AppFont.callout).foregroundStyle(Color.token("textSecondary")) }
                if let actionTitle, let action {
                    ActionButton(title: actionTitle, action: action).padding(.top, SharedTokens.spaceXs)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(SharedTokens.spaceL)
        .background(Color.toneSurface(tone), in: RoundedRectangle(cornerRadius: SharedTokens.radiusMedium, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

struct Tile: View {
    let icon: String
    let title: String
    var chip: (text: String, tone: String)? = nil
    var value: String? = nil
    var detail: String? = nil
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                HStack {
                    AppIcon(name: icon, size: 22, color: .token("accent"))
                    Spacer()
                    AppIcon(name: "chevron-right", size: 16, color: .token("textTertiary"))
                }
                Text(title).font(AppFont.headline).foregroundStyle(Color.token("textPrimary"))
                if let chip { Chip(text: chip.text, tone: chip.tone) }
                if let value { Text(value).font(AppFont.body).foregroundStyle(Color.token("textPrimary")) }
                if let detail { Text(detail).font(AppFont.caption).foregroundStyle(Color.token("textSecondary")) }
            }
            .multilineTextAlignment(.leading)
            .padding(SharedTokens.spaceL)
            .frame(maxWidth: .infinity, minHeight: 144, maxHeight: .infinity, alignment: .topLeading)
            .background(Color.token("surface"), in: RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous).strokeBorder(Color.token("border")))
            .contentShape(RoundedRectangle(cornerRadius: SharedTokens.radiusLarge, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
    }
}

struct SectionTitle: View {
    let text: String
    var body: some View {
        Text(text).font(AppFont.headline).foregroundStyle(Color.token("textPrimary"))
            .accessibilityAddTraits(.isHeader)
    }
}

/// Keep sibling preferences aligned: the checkbox labels share a baseline and all
/// descriptions begin after the checkbox, including when the text wraps.
struct SettingToggleRow: View {
    let title: String
    let detail: String
    @Binding var isOn: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: SharedTokens.spaceXs) {
            Toggle(isOn: $isOn) { Text(title).font(AppFont.body) }
                .frame(minHeight: 32)
            Text(detail).font(AppFont.callout).foregroundStyle(Color.token("textSecondary"))
                .fixedSize(horizontal: false, vertical: true)
                .lineSpacing(SharedTokens.spaceXs / 2)
                .padding(.leading, SharedTokens.sizeIconMedium)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct PageScroll<Content: View>: View {
    let title: String
    var maxWidth: CGFloat = SharedTokens.sizeContentMaxWidth
    @ViewBuilder var content: Content
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: SharedTokens.spaceL) {
                if !title.isEmpty { Text(title).font(AppFont.title).foregroundStyle(Color.token("textPrimary")).accessibilityAddTraits(.isHeader) }
                content
            }
            .frame(maxWidth: maxWidth, alignment: .leading)
            .padding(.horizontal, SharedTokens.spaceXxl)
            .padding(.vertical, SharedTokens.spaceXl)
            .frame(maxWidth: .infinity)
        }
        .background(Color.token("background"))
    }
}

/// Small "i" button with a popover: one per section, keyboard reachable, Esc closes.
/// Critical states (errors, pauses, unsaved changes, safety reasons) are never hidden in here.
struct InfoButton: View {
    let topic: String
    @State private var shown = false
    var body: some View {
        Button { shown.toggle() } label: {
            AppIcon(name: "info", size: 16, color: .token("textSecondary"))
                .frame(width: 32, height: 32)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(SharedCopy.t("info.open") + " " + SharedCopy.t("info.\(topic).title"))
        .popover(isPresented: $shown, arrowEdge: .bottom) {
            VStack(alignment: .leading, spacing: SharedTokens.spaceS) {
                Text(SharedCopy.t("info.\(topic).title")).font(AppFont.headline)
                Text(SharedCopy.t("info.\(topic).body")).font(AppFont.callout)
                    .foregroundStyle(Color.token("textSecondary"))
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Spacer()
                    Button(SharedCopy.t("info.close")) { shown = false }.keyboardShortcut(.cancelAction)
                }
            }
            .padding(SharedTokens.spaceL)
            .frame(width: 320)
        }
    }
}

/// Wraps its children onto as many rows as needed (chips, readiness items).
struct FlowLayout: Layout {
    var spacing: CGFloat = 8
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? .infinity
        var x: CGFloat = 0, y: CGFloat = 0, rowHeight: CGFloat = 0, widest: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > 0 && x + size.width > width { x = 0; y += rowHeight + spacing; rowHeight = 0 }
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
            widest = max(widest, x - spacing)
        }
        return CGSize(width: widest, height: y + rowHeight)
    }
    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, rowHeight: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > bounds.minX && x + size.width > bounds.maxX { x = bounds.minX; y += rowHeight + spacing; rowHeight = 0 }
            view.place(at: CGPoint(x: x, y: y), anchor: .topLeading, proposal: ProposedViewSize(size))
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
        }
    }
}

/// Tone icon + text: state is never carried by colour alone.
struct ToneLabel: View {
    let text: String
    let tone: String
    var body: some View {
        HStack(spacing: 5) {
            AppIcon(name: SharedTokens.tones[tone]?.icon ?? "info", size: 14, color: .tone(tone))
            Text(text).font(AppFont.caption.weight(.medium)).foregroundStyle(Color.token("textPrimary")).lineLimit(1)
        }
        .padding(.horizontal, 10)
        .frame(minHeight: SharedTokens.sizeChipHeight)
        .background(Color.toneSurface(tone), in: Capsule())
        .accessibilityElement(children: .combine)
    }
}

enum Clipboard {
    static func copy(_ text: String) {
        #if os(macOS)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        #else
        UIPasteboard.general.string = text
        #endif
    }
}

enum OfficialLinks {
    static let official: [(key: String, url: String)] = [
        ("nc", "https://www1.szu.edu.cn/nc/"), ("download", "https://www1.szu.edu.cn/nc/view.asp?id=64"),
        ("contact", "https://www1.szu.edu.cn/nc/view.asp?id=177"), ("identity", "https://www1.szu.edu.cn/v.asp?id=191"),
        ("activation", "https://authserver.szu.edu.cn/retrieve-password/accountActivation/index.html"),
        ("forgot", "https://authserver.szu.edu.cn/retrieve-password/retrievePassword/index.html"),
        ("self", "https://self.szu.edu.cn/self/nav_login"), ("teachingGuide", "https://www1.szu.edu.cn/nc/view.asp?id=641"),
        ("library", "https://www.lib.szu.edu.cn/space-and-facilities/wlan"), ("dormAccount", "https://www1.szu.edu.cn/nc/view.asp?id=249"),
        ("dormFAQ", "https://www1.szu.edu.cn/nc/view.asp?id=241"),
    ]
    static let credits: [(key: String, url: String)] = [
        ("atmluck", "https://github.com/ATMLuck/AutoSZUWeb"),
    ]
    static func url(_ key: String) -> URL? { official.first { $0.key == key }.flatMap { URL(string: $0.url) } }
}

struct LinkRow: View {
    let key: String
    @Environment(\.openURL) private var openURL
    var body: some View {
        if let url = OfficialLinks.url(key) {
            Button { openURL(url) } label: {
                HStack(spacing: SharedTokens.spaceS) {
                    Text(SharedCopy.t("link.\(key)")).underline()
                    AppIcon(name: "external-link", size: 14, color: .token("accent"))
                }
                .font(AppFont.body)
                .foregroundStyle(Color.token("accent"))
                .frame(minHeight: SharedTokens.sizeMinTarget)
            }
            .buttonStyle(.plain)
            .accessibilityAddTraits(.isLink)
        }
    }
}
