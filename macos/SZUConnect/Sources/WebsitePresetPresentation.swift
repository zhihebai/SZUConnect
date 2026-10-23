import Foundation

/// Titles follow the generated manifest. URLs have one source in SharedLinks;
/// presentation never makes requests or edits a saved website selection.
enum WebsitePresetPresentation {
    struct Item: Identifiable, Equatable {
        let key: String
        let url: String
        let addTitleKey: String
        let addedTitleKey: String
        var id: String { key }
        func titleKey(added: Bool) -> String { added ? addedTitleKey : addTitleKey }
    }

    static var items: [Item] {
        SharedLinks.websitePresets.compactMap { preset in
            switch preset.key {
            case "baidu": return Item(key: preset.key, url: preset.url,
                addTitleKey: "internet.presets.addBaidu", addedTitleKey: "internet.presets.addedBaidu")
            case "tencent": return Item(key: preset.key, url: preset.url,
                addTitleKey: "internet.presets.addTencent", addedTitleKey: "internet.presets.addedTencent")
            default: return nil
            }
        }
    }
}

struct WebsitePresetFeedback: Equatable {
    private(set) var addedKey: String?

    mutating func record(key: String, succeeded: Bool) {
        addedKey = succeeded && WebsitePresetPresentation.items.contains(where: { $0.key == key }) ? key : nil
    }

    func messageKey(contains: (String) -> Bool) -> String? {
        guard let addedKey, contains(addedKey) else { return nil }
        return WebsitePresetPresentation.items.first(where: { $0.key == addedKey })?.addedTitleKey
    }
}
