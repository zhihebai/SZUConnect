import Foundation

@main struct NetworkLabelsTests {
    static func main() throws {
        var checks = 0
        var failures: [String] = []
        func expect(_ result: @autoclosure () throws -> Bool, _ note: String) {
            checks += 1
            if (try? result()) != true { failures.append(note) }
        }
        let one = "01234567", two = "89abcdef", three = "23456789"
        var labels = NetworkLabels()
        expect(labels.displayName(for: one) == "未命名的校园网络", "legacy record must not invent a name")
        expect(labels.updateAutomatic(id: one, displayName: "SZU-Campus", kind: .wifi, ssid: "SZU-Campus"), "create observed wifi")
        expect(labels.displayName(for: one) == "SZU-Campus", "show actual SSID")
        expect(labels.entries[one]?.ssid == "SZU-Campus", "SSID stored separately")
        expect(!labels.entries[one]!.manualName, "automatic metadata")
        expect(labels.updateAutomatic(id: two, displayName: "宿舍 USB 网卡", kind: .ethernet), "create observed ethernet")
        expect(labels.displayName(for: two) == "宿舍 USB 网卡", "show system network service")
        expect(labels.sameNameIndex(for: one, among: [one, two]) == nil, "unique display names need no number")
        var sameNames = labels
        _ = sameNames.updateAutomatic(id: three, displayName: "SZU-Campus", kind: .wifi, ssid: "SZU-Campus")
        expect(sameNames.sameNameIndex(for: one, among: [one, two, three]) == 1
            && sameNames.sameNameIndex(for: three, among: [one, two, three]) == 2,
            "same names are distinguished by stable record order")
        expect(sameNames.sameNameIndex(for: one, among: [three, two, one]) == 1
            && sameNames.sameNameIndex(for: three, among: [three, two, one]) == 2,
            "refreshing list order does not renumber the same identities")
        expect(sameNames.entries.count == 3 && sameNames.entries[one]?.ssid == sameNames.entries[three]?.ssid,
            "numbering keeps independent identities and exact SSIDs")
        expect(sameNames.sameNameIndex(for: three, among: [two, three]) == nil,
            "numbering disappears when only one same-named record remains")
        expect(labels.rename(id: one, to: "宿舍的校园 Wi-Fi"), "manual rename succeeds")
        expect(labels.entries[one]?.manualName == true, "manual name marked")
        expect(labels.entries[one]?.ssid == "SZU-Campus", "rename never modifies actual SSID")
        _ = labels.updateAutomatic(id: one, displayName: "SZU-New", kind: .wifi, ssid: "SZU-New")
        expect(labels.displayName(for: one) == "宿舍的校园 Wi-Fi", "automatic metadata must preserve manual name")
        expect(labels.entries[one]?.ssid == "SZU-New", "trusted current metadata may refresh actual SSID")
        _ = labels.updateAutomatic(id: one, displayName: "以太网", kind: .ethernet)
        expect(labels.entries[one]?.ssid == "SZU-New", "ethernet observation must preserve wifi candidate")
        expect(labels.displayName(for: one) == "宿舍的校园 Wi-Fi", "ethernet preserves manual label")
        _ = labels.updateAutomatic(id: three, displayName: "SZU-WiFi", kind: .wifi, ssid: "SZU-WiFi")
        expect(!labels.updateAutomatic(id: three, displayName: "以太网", kind: .ethernet), "generic ethernet cannot overwrite known wifi")
        expect(labels.entries[three]?.kind == .wifi, "wifi metadata survives wired observation")
        expect(!labels.updateAutomatic(id: three, displayName: NetworkLabels.wifiUnnamed, kind: .wifi), "missing permission cannot replace known wifi name")
        expect(labels.entries[three]?.ssid == "SZU-WiFi", "missing permission does not delete old SSID")
        let previous = labels
        expect(!labels.rename(id: one, to: " \n\u{202e} "), "empty sanitized name rejected")
        expect(labels == previous, "rejected rename leaves entry unchanged")
        expect(!labels.rename(id: "not-an-id", to: "bad"), "invalid record ID rejected")
        expect(!labels.updateAutomatic(id: "not-an-id", displayName: "bad", kind: .other), "invalid automatic ID rejected")
        expect(NetworkLabels.cleanName("\u{202e}Campus\n\u{0000}\tWi-Fi\u{2028}") == "CampusWi-Fi", "control bidi and line separators removed")
        expect(NetworkLabels.cleanName(String(repeating: "深", count: 100)).count == 80, "name length capped in characters")
        expect(NetworkLabels.validSSID(" Campus ") == " Campus ", "SSID leading and trailing spaces retained exactly")
        expect(NetworkLabels.validSSID("\nCampus") == nil, "control character SSID rejected instead of changed")
        expect(NetworkLabels.validSSID("\u{202e}Campus") == nil, "bidi formatting SSID rejected")
        expect(NetworkLabels.validSSID(String(repeating: "深", count: 11)) == nil, "SSID limit is 32 bytes")
        expect(NetworkLabels.validSSID(String(repeating: "x", count: 32)) != nil, "32 byte SSID accepted")
        expect(NetworkLabels.validSSID("   ") == nil, "blank SSID unavailable")
        expect(NetworkLabels.decode(labels.encoded()) == labels, "sidecar round trip")
        expect(NetworkLabels.decode(Data("{broken".utf8)) == nil, "corrupt JSON unavailable")
        expect(NetworkLabels.decode(Data("{\"schema\":2,\"labels\":{}}".utf8)) == nil, "future schema unavailable")
        expect(NetworkLabels.decode(Data(repeating: 32, count: 65_537)) == nil, "oversized sidecar rejected")
        expect(NetworkLabels.decode(Data("{\"schema\":1,\"labels\":{\"bad-id\":{\"displayName\":\"name\",\"kind\":\"wifi\",\"manualName\":false}}}".utf8)) == nil, "malformed stored ID unavailable")
        expect(NetworkLabels.decode(Data("{\"schema\":1,\"labels\":{\"01234567\":{\"displayName\":\"name\",\"kind\":\"future\",\"manualName\":false}}}".utf8)) == nil, "unknown kind unavailable")
        let sanitized = Data("{\"schema\":1,\"labels\":{\"01234567\":{\"displayName\":\"Campus\\n\",\"kind\":\"wifi\",\"ssid\":\"campus\\n\",\"manualName\":false}}}".utf8)
        expect(NetworkLabels.decode(sanitized)?.displayName(for: one) == "Campus", "decoded display strings sanitized")
        expect(NetworkLabels.decode(sanitized)?.entries[one]?.ssid == nil, "decoded unsafe SSID unavailable")
        expect(labels.retain(ids: [one, two]), "forget removes sidecar metadata")
        expect(labels.entries[three] == nil && labels.entries[one] != nil, "forget retains other entries")
        expect(!labels.retain(ids: [one, two]), "unchanged prune is idempotent")
        let wifi = NetworkObservedLabel.make(kind: .wifi, serviceName: "Wi-Fi", ssid: "SZU")
        expect(wifi.displayName == "SZU" && wifi.ssid == "SZU", "observed wifi uses real SSID")
        let unknownWiFi = NetworkObservedLabel.make(kind: .wifi, serviceName: "Wi-Fi")
        expect(unknownWiFi.displayName == NetworkLabels.wifiUnnamed && unknownWiFi.ssid == nil, "missing SSID never uses adapter label as real network")
        expect(NetworkObservedLabel.make(kind: .ethernet, serviceName: "USB 10/100/1000 LAN").displayName == "USB 10/100/1000 LAN", "ethernet uses system service name")
        expect(NetworkObservedLabel.make(kind: .other).displayName == NetworkLabels.unnamed, "unknown attachment fallback")

        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("szu-label-tests-" + UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = NetworkLabelsStore(url: directory.appendingPathComponent(NetworkLabelsStore.filename))
        if case .missing = store.load() { expect(true, "missing file") } else { expect(false, "missing file") }
        expect(store.save(labels), "new sidecar saves")
        if case .loaded(let restored) = store.load() { expect(restored == labels, "stored content") } else { expect(false, "stored content") }
        let permissions = try FileManager.default.attributesOfItem(atPath: store.url.path)[.posixPermissions] as? NSNumber
        expect(permissions?.intValue == 0o600, "sidecar mode 0600")
        expect(try FileManager.default.contentsOfDirectory(atPath: directory.path).count == 1, "no temporary files left")
        var updated = labels
        _ = updated.rename(id: one, to: "新名")
        expect(store.save(updated), "atomic update")
        if case .loaded(let restored) = store.load() { expect(restored == updated, "updated content") } else { expect(false, "updated content") }
        let unknown = Data("{\"schema\":88,\"labels\":{}}".utf8)
        try unknown.write(to: store.url)
        if case .unavailable = store.load() { expect(true, "unknown schema protected") } else { expect(false, "unknown schema protected") }
        expect(!store.save(labels), "future schema cannot be overwritten")
        expect(try Data(contentsOf: store.url) == unknown, "future schema bytes preserved")
        let corrupt = Data("preserve corrupt bytes".utf8)
        try corrupt.write(to: store.url)
        expect(!store.save(labels), "corrupt sidecar cannot be overwritten")
        expect(try Data(contentsOf: store.url) == corrupt, "corrupt original preserved")
        try FileManager.default.removeItem(at: store.url)
        let target = directory.appendingPathComponent("target.json")
        try labels.encoded().write(to: target)
        try FileManager.default.createSymbolicLink(at: store.url, withDestinationURL: target)
        expect(!store.save(updated), "symlink must not be replaced or followed for saving")
        expect(try Data(contentsOf: target) == labels.encoded(), "symlink target unchanged")
        if failures.isEmpty { print("PASS NetworkLabelsTests: \(checks) checks") }
        else { for failure in failures { print("FAIL: \(failure)") }; exit(1) }
    }
}
