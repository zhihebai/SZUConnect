import AppKit
import SwiftUI

@main
struct DesignTokenColorTests {
    @MainActor static func main() {
        var assertions = 0
        func check(_ condition: Bool, _ message: String) {
            precondition(condition, message)
            assertions += 1
        }
        let light = NSAppearance(named: .aqua)!
        let dark = NSAppearance(named: .darkAqua)!
        func components(_ color: Color, appearance: NSAppearance) -> [Double] {
            var result: [Double] = []
            appearance.performAsCurrentDrawingAppearance {
                let resolved = NSColor(color).usingColorSpace(.sRGB)!
                result = [resolved.redComponent, resolved.greenComponent, resolved.blueComponent, resolved.alphaComponent]
            }
            return result
        }
        func matches(_ color: Color, _ expected: SharedTokens.RGBA, appearance: NSAppearance) -> Bool {
            zip(components(color, appearance: appearance), [expected.r, expected.g, expected.b, expected.a])
                .allSatisfy { abs($0 - $1) < 0.00001 }
        }
        for (name, pair) in SharedTokens.colors {
            let color = Color.token(name)
            check(color == Color.token(name), "Repeated tokens have stable identity: \(name)")
            check(matches(color, pair.light, appearance: light), "Light appearance: \(name)")
            check(matches(color, pair.dark, appearance: dark), "Dark appearance after light: \(name)")
            check(matches(color, pair.light, appearance: light), "Light appearance after dark: \(name)")
        }
        check(Color.token("unknown-token") == .pink, "Unknown tokens keep the visible fallback")
        for (name, tone) in SharedTokens.tones {
            check(Color.tone(name) == Color.token(tone.foreground), "Tone foreground: \(name)")
            check(Color.toneSurface(name) == Color.token(tone.surface), "Tone surface: \(name)")
        }
        print("PASS \(assertions) color identity and appearance assertions; no system settings changed")
    }
}
