import Foundation

@main
struct WebsitePresetPresentationTests {
    static func main() {
        var passed = 0
        func check(_ condition: Bool, _ name: String) {
            precondition(condition, name); passed += 1; print("PASS: " + name)
        }
        let items = WebsitePresetPresentation.items
        check(items.map(\.key) == ["baidu", "tencent"], "Only the two reviewed optional presets appear in manifest order")
        check(Set(items.map(\.id)).count == 2, "Preset controls have distinct stable accessibility identities")
        check(items.map(\.url) == SharedLinks.websitePresets.map(\.url), "Presentation reads the generated URL manifest without another stored URL list")
        check(items[0].url == "https://www.baidu.com/" && items[1].url == "https://www.qq.com/",
              "The reviewed preset destinations retain TLS and the expected complete URLs")
        check(items[0].titleKey(added: false) == "internet.presets.addBaidu"
              && items[1].titleKey(added: false) == "internet.presets.addTencent", "Available presets offer addition through the existing copy keys")
        check(items[0].titleKey(added: true) == "internet.presets.addedBaidu"
              && items[1].titleKey(added: true) == "internet.presets.addedTencent", "Existing presets show the distinct already-added labels")
        var feedback = WebsitePresetFeedback()
        check(feedback.messageKey(contains: { _ in true }) == nil, "A fresh editor makes no claim that it added a preset")
        feedback.record(key: "baidu", succeeded: true)
        check(feedback.addedKey == "baidu" && feedback.messageKey(contains: { $0 == "baidu" }) == "internet.presets.addedBaidu",
              "A successful explicit addition shows feedback for that preset")
        check(feedback.messageKey(contains: { _ in false }) == nil, "Removing a preset hides its now-stale success feedback")
        feedback.record(key: "tencent", succeeded: true)
        check(feedback.messageKey(contains: { $0 == "tencent" }) == "internet.presets.addedTencent",
              "A second successful addition updates feedback without changing the first preset")
        feedback.record(key: "baidu", succeeded: false)
        check(feedback.addedKey == nil && feedback.messageKey(contains: { _ in true }) == nil,
              "A rejected addition cannot leave a false success message")
        feedback.record(key: "unknown", succeeded: true)
        check(feedback.addedKey == nil, "Unreviewed preset keys cannot invent presentation or feedback")
        for key in ["internet.presets.title", "internet.presets.addBaidu", "internet.presets.addTencent",
                    "internet.presets.addedBaidu", "internet.presets.addedTencent", "internet.presets.detail"] {
            let value = SharedCopy.t(key)
            check(!value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && value != key,
                  "The generated Mac preset copy is present: " + key)
        }
        print("\(passed) website preset presentation assertions passed; no requests or application data were accessed")
    }
}
