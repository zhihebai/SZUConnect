import AppKit
import SwiftUI

@main
struct HelpPresentationTests {
    @MainActor static func main() throws {
        var passed = 0
        func check(_ condition: Bool, _ name: String) {
            guard condition else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        let state = HelpDisclosureState()
        check(!state.firstUseExpanded && state.expandedFAQs.isEmpty, "new help session starts collapsed")
        state.faq("wifi").wrappedValue = true
        check(!state.firstUseExpanded && state.faq("wifi").wrappedValue, "FAQ expansion leaves first use collapsed")
        state.firstUseExpanded = true
        state.faq("cleanup").wrappedValue = true
        state.faq("wifi").wrappedValue = false
        check(state.firstUseExpanded && state.faq("cleanup").wrappedValue && !state.faq("wifi").wrappedValue,
              "first use and individual FAQ choices remain independent")
        let reopenedSession = state
        check(reopenedSession.firstUseExpanded && reopenedSession.faq("cleanup").wrappedValue,
              "dashboard-owned session keeps choices when help is reopened")
        let otherSession = HelpDisclosureState()
        check(!otherSession.firstUseExpanded && otherSession.expandedFAQs.isEmpty,
              "another session does not inherit previous choices or write preferences")

        let source = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
        let copy = try JSONSerialization.jsonObject(with: source) as! [String: Any]
        func text(_ path: String) -> String {
            var value: Any = copy
            for key in path.split(separator: ".") { value = (value as! [String: Any])[String(key)]! }
            return (value as? String) ?? (value as! [String: String])["mac"]!
        }
        func lineCount(_ text: String, width: CGFloat, size: CGFloat) -> Int {
            let storage = NSTextStorage(string: text, attributes: [.font: NSFont.systemFont(ofSize: size)])
            let layout = NSLayoutManager()
            let container = NSTextContainer(size: NSSize(width: width, height: .greatestFiniteMagnitude))
            container.lineFragmentPadding = 0
            storage.addLayoutManager(layout); layout.addTextContainer(container)
            layout.ensureLayout(for: container)
            var count = 0
            layout.enumerateLineFragments(forGlyphRange: NSRange(location: 0, length: layout.numberOfGlyphs)) { _, _, _, _, _ in count += 1 }
            return count
        }
        let compactKeys = ["internet.targets.detail", "internet.strategyScope", "website.strategy.system.detail",
            "website.strategy.direct.detail", "setup.helpAccount", "setup.helpCheck", "setup.helpMemory",
            "setup.helpRecovery", "setup.wifi.body", "help.faq.wifi.a", "help.faq.cleanup.a", "help.faq.vpn.a"]
        for key in compactKeys {
            for (index, paragraph) in text(key).components(separatedBy: "\n\n").enumerated() {
                for size: CGFloat in [14, 20] {
                    check(lineCount(paragraph, width: 404, size: size) <= 2,
                          "compact text fits two lines at \(size)pt: \(key) paragraph \(index)")
                }
            }
        }
        check(text("info.proxyModes.body").contains("macOS") && !text("info.proxyModes.body").contains("Windows"),
              "Mac proxy explanation describes its own system")
        let simultaneousProxyNote = "同时开启系统代理与 TUN 不保证更快。"
        check(text("info.proxyModes.body").components(separatedBy: "\n\n").contains(simultaneousProxyNote),
              "Proxy and TUN overlap is explained in its own short paragraph")
        for size: CGFloat in [14, 20] {
            check(lineCount(simultaneousProxyNote, width: 404, size: size) <= 2,
                  "Proxy and TUN overlap remains readable in compact large-type layout at \(size)pt")
        }
        check(text("help.faq.wifi.a").contains("仅支持无需无线密码"), "Mac help retains encrypted Wi-Fi platform limit")
        check(text("help.faq.cleanup.a").contains("不注销校园账号、不主动断网"), "account deletion is distinct from network logout")
        print("\(passed) help presentation assertions passed")
    }
}
