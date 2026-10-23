import Foundation

// Runs the shared JSON contracts against the Swift ports (same fixtures as the C++ core tests).
// Usage: run-shared-contract.sh (passes the repository root that contains shared/).
@main enum SharedContractTests {
    static var failures = 0
    static var passed = 0
    static func check(_ condition: Bool, _ message: @autoclosure () -> String) {
        if condition { passed += 1 } else { failures += 1; print("FAIL:", message()) }
    }
    static func load(_ root: URL, _ path: String) throws -> [String: Any] {
        let data = try Data(contentsOf: root.appendingPathComponent(path))
        return try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:]
    }

    static func main() throws {
        let root = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "../..")
        // 1. Status presentation (S01–S31)
        let status = try load(root, "shared/status-fixtures.json")
        for item in status["cases"] as? [[String: Any]] ?? [] {
            let id = item["id"] as? String ?? "?"
            let i = item["input"] as? [String: Any] ?? [:]
            var input = StatusInput()
            input.status = i["status"] as? String ?? "idle"
            input.diagnosis = i["diagnosis"] as? String
            input.probeMode = i["probeMode"] as? String ?? "quick"
            input.passedCount = i["passedCount"] as? Int ?? 0
            input.mode = i["mode"] as? String ?? "manual"
            input.readOnly = i["readOnly"] as? Bool ?? false
            input.hasDraft = i["hasDraft"] as? Bool ?? false
            input.autoLogin = i["autoLogin"] as? String ?? "off"
            input.hold = i["hold"] as? String
            input.offerForcedLogin = i["offerForcedLogin"] as? Bool ?? false
            let p = StatusPresenter.present(input)
            let e = item["expected"] as? [String: Any] ?? [:]
            check(p.tone == e["tone"] as? String, "\(id) tone \(p.tone)")
            check(p.headline == e["headline"] as? String, "\(id) headline \(p.headline)")
            check(p.detail == e["detail"] as? String, "\(id) detail \(p.detail)")
            check(p.primary == e["primary"] as? String, "\(id) primary \(String(describing: p.primary))")
            check(p.secondary == e["secondary"] as? String, "\(id) secondary \(String(describing: p.secondary))")
            check(p.chip == e["chip"] as? String, "\(id) chip \(p.chip)")
            check(p.chipTone == e["chipTone"] as? String, "\(id) chipTone \(p.chipTone)")
            let banner = e["banner"] as? [String: Any]
            check(p.banner?.key == banner?["key"] as? String && p.banner?.tone == banner?["tone"] as? String
                  && p.banner?.action == banner?["action"] as? String, "\(id) banner")
        }
        // 2. Rejection classification
        let rejections = try load(root, "shared/fixtures/rejection-classification-cases.json")
        for item in rejections["cases"] as? [[String: Any]] ?? [] {
            let object = item["response"] as? [String: Any] ?? item["object"] as? [String: Any] ?? [:]
            let expected = item["expected"] as? String ?? item["kind"] as? String
            check(PortalWire.rejectionKind(object).rawValue == expected, "rejection \(item["id"] ?? "?")")
        }
        // 3. Proxy matcher and summary
        let proxy = try load(root, "shared/fixtures/proxy-environment-cases.json")
        for item in proxy["matcherCases"] as? [[String: Any]] ?? [] {
            let pattern = item["pattern"] as? String ?? "", host = item["host"] as? String ?? ""
            check(ProxyRules.patternMatches(pattern, host: host) == (item["matches"] as? Bool ?? false), "matcher \(pattern) ~ \(host)")
        }
        for item in proxy["summaryCases"] as? [[String: Any]] ?? [] {
            let s = item["snapshot"] as? [String: Any] ?? [:]
            var snapshot = ProxySnapshot(readable: s["readable"] as? Bool ?? false)
            snapshot.http = s["http"] as? Bool ?? false
            snapshot.https = s["https"] as? Bool ?? false
            snapshot.socks = s["socks"] as? Bool ?? false
            snapshot.pac = s["pac"] as? Bool ?? false
            snapshot.autoDiscovery = s["autoDiscovery"] as? Bool ?? false
            snapshot.exceptions = s["exceptions"] as? [String] ?? []
            let summary = ProxyRules.summarize(snapshot)
            let e = item["expected"] as? [String: Any] ?? [:]
            check(summary.state == e["state"] as? String && summary.kinds == (e["kinds"] as? [String] ?? [])
                  && summary.campusBypass == e["campusBypass"] as? String, "summary \(item["id"] ?? "?")")
        }
        // 4. Compatibility diagnosis
        let compat = try load(root, "shared/fixtures/compat-diagnosis-cases.json")
        for item in compat["cases"] as? [[String: Any]] ?? [] {
            let i = item["input"] as? [String: Any] ?? [:]
            let input = CompatInput(proxy: i["proxy"] as? String ?? "unknown", tunnel: i["tunnel"] as? String ?? "unknown",
                                    campusDirect: i["campusDirect"] as? String ?? "notRun", campusSystem: i["campusSystem"] as? String ?? "notRun",
                                    publicDirect: i["publicDirect"] as? String ?? "notRun", publicSystem: i["publicSystem"] as? String ?? "notRun")
            let v = CompatDiagnosis.diagnose(input)
            let e = item["expected"] as? [String: Any] ?? [:]
            check(v.verdict == e["verdict"] as? String && v.recommendations == (e["recommendations"] as? [String] ?? [])
                  && v.offerRules == (e["offerRules"] as? Bool ?? false), "compat \(item["id"] ?? "?") → \(v.verdict)")
        }
        check(ProxyRules.rulesText == "- DOMAIN,net.szu.edu.cn,DIRECT\n- IP-CIDR,172.30.255.42/32,DIRECT,no-resolve", "Clash rules text")
        print("\(passed) shared-contract assertions passed, \(failures) failed.")
        if failures > 0 { exit(1) }
    }
}
