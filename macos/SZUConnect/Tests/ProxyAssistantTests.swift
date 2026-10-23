import Foundation
#if os(macOS)
final class MemoryProxyVault: ProxyAssistantKeyVault {
    var value: Data?
    var failLoad = false, failCreate = false, failRemove = false
    var loads = 0, creates = 0, removes = 0
    func load() throws -> Data? { loads += 1; if failLoad { throw ProxyAssistantFailure("backupCorrupt") }; return value }
    func create(_ key: Data) throws { creates += 1; guard !failCreate, value == nil else { throw ProxyAssistantFailure("backupFailed") }; value = key }
    func remove() throws { removes += 1; guard !failRemove else { throw ProxyAssistantFailure("keyCleanup") }; value = nil }
}
@main struct ProxyAssistantTests {
    static var checks = 0
    static func expect(_ condition: Bool, _ message: String) { checks += 1; if !condition { fatalError(message) } }
    static func reject(_ message: String, token: String? = nil, _ action: () throws -> Void) {
        checks += 1
        do { try action(); fatalError("accepted: " + message) } catch let failure as ProxyAssistantFailure { if let token { expect(failure.token == token, message + ": " + failure.token) } } catch { fatalError("unexpected error: " + message) }
    }
    static func patch(_ text: String, _ format: ProxyAssistantFormat) throws -> String { String(decoding: try ProxyAssistantConfig.patch(Data(text.utf8), format: format).bytes, as: UTF8.self) }
    static func main() throws {
        setbuf(stdout, nil)
        let v2rayn = try JSONSerialization.jsonObject(with: Data(ProxyAssistantConfig.v2raynRules.utf8)) as! [[String: Any]]
        expect(v2rayn.count == 2, "v2rayN clipboard contains exactly two routing rules")
        for rule in v2rayn {
            let selector = rule["domain"] != nil ? "domain" : "ip"
            expect(Set(rule.keys) == Set(["remarks", "enabled", "ruleType", "outboundTag", selector]),
                   "v2rayN rule contains only the bounded routing editor fields")
            expect(rule["enabled"] as? Bool == true && rule["ruleType"] as? Int == 1 && rule["outboundTag"] as? String == "direct",
                   "v2rayN rules select enabled direct field rules")
        }
        expect(v2rayn[0]["domain"] as? [String] == ["full:net.szu.edu.cn"], "v2rayN domain match is exact")
        expect(v2rayn[1]["ip"] as? [String] == ["172.30.255.42/32"], "v2rayN address match is a single fixed host")
        expect(ProxyAssistantConfig.v2raynRules.utf8.count < 1024, "v2rayN routing import stays bounded")
        // Export only the fixed production script, never a user-selected script.
        if CommandLine.arguments.count == 2 { try Data(ProxyAssistantConfig.script.utf8).write(to: URL(fileURLWithPath: CommandLine.arguments[1])) }
        let script = try patch("// official empty script\nfunction main(config, profileName) {\n return config;\n}\n", .clashScript)
        expect(script == ProxyAssistantConfig.script, "default script")
        expect(try patch(script, .clashScript) == script, "script idempotent")
        for source in ["function main(config) { return\nconfig; }", "function main(config) { config.x = 1; return config; }", "function main(config) { return/*\n*/config; }", "/* unfinished", "function main(config) { return config } alert(1)"] {
            reject("custom script", token: "unsupportedScript") { _ = try patch(source, .clashScript) }
        }
        let yaml = """
        mode: rule
        unknown-id: 99999999999999999999999999999999999999
        unknown-string: '123'
        url: https://example.invalid/config#anchor
        dns:
          enable: true
          enhanced-mode: fake-ip
          fake-ip-filter-mode: blacklist
          fake-ip-filter:
            - example.invalid
        tun:
          enable: true
          auto-route: true
          strict-route: true
          route-exclude-address:
            - 192.0.2.0/24
        rules:
          - MATCH,Other
        """
        let result = try patch(yaml, .mihomo)
        expect(result.contains("99999999999999999999999999999999999999"), "YAML huge integer preserved")
        expect(result.contains("\"unknown-string\":\"123\""), "YAML numeric string preserved")
        expect(result.replacingOccurrences(of: "\\/", with: "/").contains("https://example.invalid/config#anchor"), "YAML URL fragment preserved")
        expect(result.contains("\"strict-route\":true"), "strict route retained")
        expect(result.replacingOccurrences(of: "\\/", with: "/").contains("192.0.2.0/24"), "old TUN exclusion retained")
        expect(try patch(result, .mihomo) == result, "mihomo idempotent")
        let ruleMode = try patch("{\"dns\":{\"enhanced-mode\":\"fake-ip\",\"fake-ip-filter-mode\":\"rule\",\"fake-ip-filter\":[\"MATCH,fake-ip\"]}}", .mihomo)
        expect(ruleMode.contains("DOMAIN,net.szu.edu.cn,real-ip"), "rule mode real-IP priority")
        for source in ["mode: rule\nmode: global", "root: &a 1\ncopy: *a", "---\nmode: rule", "mode: rule\nunknown: !!str 1", "mode: rule\nvalue: |\n  text", "dns:\n  enhanced-mode: fake-ip\n  fake-ip-filter-mode: whitelist", "{\"mode\":\"global\"}", "{\"dns\":{\"enable\":\"true\"}}", "{\"dns\":{\"fake-ip-filter-mode\":\"rule\",\"enhanced-mode\":\"fake-ip\",\"fake-ip-filter\":123}}"] {
            reject("Mihomo invalid/unsafe") { _ = try patch(source, .mihomo) }
        }
        let sing = """
        {"unknown":99999999999999999999999999999999999,"str":"009","outbounds":[{"type":"direct","tag":"actual-direct"}],"dns":{"servers":[{"type":"fakeip","tag":"fake"},{"type":"local","tag":"campus-local"}],"rules":[{"domain":["example.invalid"],"server":"fake"}]},"inbounds":[{"type":"tun","auto_route":true,"strict_route":true,"route_exclude_address":["192.0.2.0/24"]}]}
        """
        let singResult = try patch(sing, .singBox)
        expect(singResult.contains("99999999999999999999999999999999999"), "JSON big integer retained")
        expect(singResult.contains("\"outbound\":\"actual-direct\""), "real direct tag")
        expect(singResult.contains("\"server\":\"campus-local\""), "real DNS tag")
        expect(singResult.contains("\"strict_route\":true"), "singbox strict route retained")
        expect(try patch(singResult, .singBox) == singResult, "singbox idempotent")
        for source in ["{\"outbounds\":[],\"outbounds\":[]}", "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"d\",\"tag\":\"x\"}]}", "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"a\"},{\"type\":\"direct\",\"tag\":\"b\"}]}", "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"d\",\"detour\":\"proxy\"}]}", "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"d\"}],\"dns\":{\"servers\":[{\"type\":\"fakeip\",\"tag\":\"f\"}]}}", "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"d\"}],\"unknown\":NaN}"] {
            reject("singbox unsafe") { _ = try patch(source, .singBox) }
        }
        // JSON edits preserve all untouched literal bytes, including unrepresentable numeric IDs.
        let lexical = #"{"unknown":{"big":184467440737095516160001,"exp":1e999,"negative":-0,"nested":[null,true,"009","\u4e2d"]},"rules":[]}"#
        let lexicalResult = try patch(lexical, .mihomo)
        expect(lexicalResult.contains(#""unknown":{"big":184467440737095516160001,"exp":1e999,"negative":-0,"nested":[null,true,"009","\u4e2d"]}"#), "unknown JSON values retain raw lexemes")
        for source in [#"{"rules":[],"ru\u006ces":[]}"#, #"{"x":{"k":1,"k":2}}"#, #"{"x":01}"#, #"{"x":1.}"#, #"{"x":+1}"#, #"{"x":"\uD800"}"#, #"{"rules":[],}"#, #"{"rules":[,]}"#] {
            reject("strict JSON tokens and duplicate decoded keys") { _ = try patch(source, .mihomo) }
        }
        let disabledDNS = try patch(#"{"dns":{"enable":false,"enhanced-mode":"fake-ip","fake-ip-filter-mode":"whitelist","fake-ip-filter":["original.invalid"]},"tun":{"enable":false,"auto-route":true,"strict-route":true}}"#, .mihomo)
        expect(disabledDNS.contains(#""fake-ip-filter":["original.invalid"]"#), "disabled DNS unchanged")
        expect(!disabledDNS.contains("route-exclude-address"), "disabled TUN not changed")
        for value in ["on", "YES", "0123", "0x123", ".NaN", "2026-01-01", "? complex", "- nested", "foo \" # comment \"", "foo ' # comment '", "'a' 'b'", "'a'b'c'"] {
            reject("ambiguous YAML scalar") { _ = try patch("mode: rule\nx: " + value, .mihomo) }
        }
        let mappingSequence = try patch("proxies:\n  - name: fixture-proxy\n    server: 192.0.2.1\n    port: 1080\nrules: []", .mihomo)
        expect(mappingSequence.contains(#""proxies":[{"name":"fixture-proxy","server":"192.0.2.1","port":1080}]"#), "YAML mapping sequence preserves scalar kinds")
        expect(try patch("name: 'a''b'", .mihomo).contains(#""name":"a'b""#), "YAML escaped single quote retained")
        expect(try patch("\u{FEFF}{\"rules\":[]}", .mihomo).contains("DOMAIN,net.szu.edu.cn,DIRECT"), "UTF8 BOM JSON parsed without inventing key")
        expect(try patch("\u{FEFF}mode: rule", .mihomo).contains(#""mode":"rule""#), "UTF8 BOM YAML key parsed correctly")
        expect(try patch("\u{00A0}private-key: retained", .mihomo).contains("\u{00A0}private-key"), "non-ASCII leading space remains part of YAML key")
        reject("YAML raw control") { _ = try patch("value: abc\u{001F}def", .mihomo) }
        let huge = String(repeating: "9", count: 4096)
        expect(try patch("id: " + huge, .mihomo).contains(huge), "bounded giant numeric token not converted")
        let legacy = try patch(#"{"outbounds":[{"type":"direct","tag":"d"}],"dns":{"servers":[{"address":"fakeip","tag":"f"},{"address":"local","tag":"l"}]}}"#, .singBox)
        expect(legacy.contains(#""server":"l""#) && !legacy.contains(#""action":"route""#), "legacy DNS keeps legacy schema")
        for source in [#"{"outbounds":[{"type":"direct","tag":"d"}],"dns":{"servers":[{"type":"local","tag":"l"},{"address":"local","tag":"l2"}]}}"#, #"{"outbounds":[{"type":"direct","tag":"d"}],"dns":{"servers":[{"type":"fakeip","tag":"f"},{"type":"local","tag":"l","detour":"proxy"}]}}"#, #"{"outbounds":[{"type":"direct","tag":"d"}],"inbounds":[{"type":"tun","auto_route":true}]}"#] {
            reject("mixed or insufficient sing-box schema") { _ = try patch(source, .singBox) }
        }
        reject("UTF8", token: "encoding") { _ = try ProxyAssistantConfig.patch(Data([0xFF]), format: .mihomo) }
        reject("size", token: "tooLarge") { _ = try patch(String(repeating: " ", count: 1048577), .mihomo) }
        reject("depth") { _ = try patch(String(repeating: "[", count: 50) + "1" + String(repeating: "]", count: 50), .mihomo) }
        let root = URL(fileURLWithPath: "/private/tmp/szu-proxy-tests-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: root) }
        let config = root.appendingPathComponent("config.yaml"), vault = MemoryProxyVault()
        let original = Data(yaml.utf8)
        try original.write(to: config)
        let storage = root.appendingPathComponent("backup", isDirectory: true)
        let files = ProxyAssistantFiles(directory: storage, vault: vault)
        let preview = try files.preview(config, format: .mihomo)
        expect(try Data(contentsOf: config) == original, "preview never writes")
        try files.save(preview)
        expect(files.hasBackup && vault.value?.count == 32, "encrypted backup exists")
        let encrypted = try Data(contentsOf: storage.appendingPathComponent("proxy-direct-v1.aesgcm"))
        expect(encrypted.range(of: original) == nil, "backup has no original plaintext")
        expect(encrypted.range(of: Data(config.path.utf8)) == nil, "backup has no path plaintext")
        reject("second save", token: "backupExists") { try files.save(preview) }
        // A fresh object and vault prove restoration does not depend on an in-memory preview.
        let next = ProxyAssistantFiles(directory: storage, vault: vault)
        try next.restore()
        expect(try Data(contentsOf: config) == original, "original bytes restored")
        expect(!next.hasBackup && vault.value == nil, "restored backup/key removed")
        let stale = try files.preview(config, format: .mihomo)
        try Data("mode: rule\nnew: value".utf8).write(to: config)
        reject("stale preview", token: "changed") { try files.save(stale) }
        expect(!files.hasBackup, "conflict creates no backup")
        try original.write(to: config)
        try files.save(files.preview(config, format: .mihomo))
        let edited = Data("mode: rule\nnew: value".utf8); try edited.write(to: config)
        reject("restore conflict", token: "restoreConflict") { try files.restore() }
        expect(try Data(contentsOf: config) == edited && files.hasBackup, "conflict retains target and backup")
        try files.discardBackup()
        expect(try Data(contentsOf: config) == edited, "discard leaves target")
        let keyOperations = (vault.loads, vault.creates, vault.removes)
        let readOnly = ProxyAssistantFiles(directory: storage, vault: vault, readOnly: true)
        reject("readonly preview") { _ = try readOnly.preview(config, format: .mihomo) }
        reject("readonly save") { try readOnly.save(preview) }
        reject("readonly restore") { try readOnly.restore() }
        reject("readonly discard") { try readOnly.discardBackup() }
        expect(keyOperations == (vault.loads, vault.creates, vault.removes), "read-only calls never contact vault")
        // Backup creation failures leave the selected file untouched.
        try original.write(to: config)
        let failingVault = MemoryProxyVault(); failingVault.failCreate = true
        let failedStorage = root.appendingPathComponent("failed-backup")
        let failureFiles = ProxyAssistantFiles(directory: failedStorage, vault: failingVault)
        reject("key creation failure", token: "backupFailed") { try failureFiles.save(failureFiles.preview(config, format: .mihomo)) }
        expect(try Data(contentsOf: config) == original && !failureFiles.hasBackup, "key failure never changes target")
        let external = root.appendingPathComponent("external"), unsafe = root.appendingPathComponent("unsafe")
        try FileManager.default.createDirectory(at: external, withIntermediateDirectories: false)
        try FileManager.default.createSymbolicLink(at: unsafe, withDestinationURL: external)
        let unsafeFiles = ProxyAssistantFiles(directory: unsafe.appendingPathComponent("created"), vault: MemoryProxyVault())
        reject("backup ancestor symlink") { try unsafeFiles.save(unsafeFiles.preview(config, format: .mihomo)) }
        expect(!FileManager.default.fileExists(atPath: external.appendingPathComponent("created").path), "rejected link creates nothing behind it")
        let identicalPreview = try files.preview(config, format: .mihomo)
        try FileManager.default.moveItem(at: config, to: root.appendingPathComponent("old-inode.yaml")); try original.write(to: config)
        reject("same bytes at replaced inode", token: "changed") { try files.save(identicalPreview) }
        // A failed key removal remains explicitly visible and retryable after a restart.
        try files.save(files.preview(config, format: .mihomo))
        vault.failRemove = true
        let saved = try Data(contentsOf: config)
        reject("key deletion failure", token: "keyCleanup") { try files.discardBackup() }
        expect(files.hasBackup && vault.value != nil, "key cleanup failure retains retry state")
        expect(try Data(contentsOf: config) == saved, "discard failure does not restore or alter target")
        let pending = ProxyAssistantFiles(directory: storage, vault: vault)
        reject("restore cannot misreport discarded backup", token: "keyCleanup") { try pending.restore() }
        reject("pending cleanup blocks another save", token: "backupExists") { try pending.save(preview) }
        vault.failRemove = false; try pending.discardBackup()
        expect(!pending.hasBackup && vault.value == nil, "retry clears only pending backup/key")
        expect(try Data(contentsOf: config) == saved, "cleanup retry preserves selected target")
        // Corrupted encrypted data is retained and never used to overwrite a selected file.
        try original.write(to: config); try files.save(files.preview(config, format: .mihomo))
        let backupPath = storage.appendingPathComponent("proxy-direct-v1.aesgcm")
        let validCiphertext = try Data(contentsOf: backupPath)
        var corrupt = validCiphertext; corrupt[corrupt.count / 2] ^= 0x80
        try corrupt.write(to: backupPath)
        reject("corrupted backup restore", token: "backupCorrupt") { try files.restore() }
        reject("corrupted backup discard", token: "backupCorrupt") { try files.discardBackup() }
        expect(try files.hasBackup && Data(contentsOf: config) != original, "corrupt backup keeps target and recovery evidence")
        try validCiphertext.write(to: backupPath); try files.restore()
        expect(try Data(contentsOf: config) == original, "authenticated original restores after backup repaired")
        // A failure after the durable backup keeps the old target and a usable restore path.
        if geteuid() != 0 {
            let blockedPreview = try files.preview(config, format: .mihomo)
            try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: root.path)
            do {
                reject("target directory not writable", token: "writeFailed") { try files.save(blockedPreview) }
                expect(try Data(contentsOf: config) == original && files.hasBackup, "write failure preserves target and encrypted backup")
                try files.restore()
                expect(!files.hasBackup && vault.value == nil, "failed save recovery only clears backup and key")
            }
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: root.path)
        }
        let wideBackup = root.appendingPathComponent("wide-backup")
        try FileManager.default.createDirectory(at: wideBackup, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o755])
        let unsafePermissions = ProxyAssistantFiles(directory: wideBackup, vault: MemoryProxyVault())
        reject("backup permissions too broad", token: "backupFailed") { try unsafePermissions.save(unsafePermissions.preview(config, format: .mihomo)) }
        expect(try Data(contentsOf: config) == original, "insecure backup directory leaves target unchanged")
        let symlink = root.appendingPathComponent("link.yaml")
        try FileManager.default.createSymbolicLink(at: symlink, withDestinationURL: config)
        reject("symlink") { _ = try files.preview(symlink, format: .mihomo) }
        let hardlink = root.appendingPathComponent("hard.yaml")
        try FileManager.default.linkItem(at: config, to: hardlink)
        reject("hardlink") { _ = try files.preview(config, format: .mihomo) }
        try FileManager.default.removeItem(at: hardlink)
        try FileManager.default.setAttributes([.posixPermissions: 0o400], ofItemAtPath: config.path)
        reject("readonly file") { _ = try files.preview(config, format: .mihomo) }
        print("proxy assistant: \(checks) isolated checks passed")
    }
}
#endif
