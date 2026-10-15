import Foundation
import Combine
#if canImport(CFNetwork)
import CFNetwork
#endif

// Read-only proxy facts and the shared compatibility diagnosis (shared/fixtures/proxy-environment-cases.json,
// compat-diagnosis-cases.json). The app never changes proxy, VPN, DNS or routes.
struct ProxySnapshot: Equatable {
    var readable = false
    var http = false, https = false, socks = false, pac = false, autoDiscovery = false
    var exceptions: [String] = []
}

struct ProxySummary: Equatable {
    var state: String         // unknown | off | on
    var kinds: [String]       // http https socks pac autoDiscovery
    var campusBypass: String  // unknown | notApplicable | all | partial | none
}

enum ProxyRules {
    static let campusHosts = ["net.szu.edu.cn", "172.30.255.42"]
    static let directRules = ["- DOMAIN,net.szu.edu.cn,DIRECT", "- IP-CIDR,172.30.255.42/32,DIRECT,no-resolve"]
    static var rulesText: String { directRules.joined(separator: "\n") }

    private static func ipv4(_ text: String) -> UInt32? {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        var value: UInt32 = 0
        for part in parts {
            guard !part.isEmpty, part.count <= 3, part.allSatisfy({ $0.isASCII && $0.isNumber }), let octet = UInt32(part), octet <= 255 else { return nil }
            value = (value << 8) | octet
        }
        return value
    }

    private static func glob(_ pattern: [Character], _ text: [Character]) -> Bool {
        var p = 0, t = 0, star = -1, mark = 0
        while t < text.count {
            if p < pattern.count && (pattern[p] == text[t]) { p += 1; t += 1 }
            else if p < pattern.count && pattern[p] == "*" { star = p; mark = t; p += 1 }
            else if star >= 0 { p = star + 1; mark += 1; t = mark }
            else { return false }
        }
        while p < pattern.count && pattern[p] == "*" { p += 1 }
        return p == pattern.count
    }

    /// Windows-style bypass entries; macOS ExceptionsList uses the same shapes.
    static func patternMatches(_ rawPattern: String, host rawHost: String) -> Bool {
        let pattern = rawPattern.trimmingCharacters(in: .whitespaces).lowercased()
        let host = rawHost.lowercased()
        guard !pattern.isEmpty else { return false }
        if pattern == "<local>" { return !host.contains(".") && !host.contains(":") }
        if let slash = pattern.firstIndex(of: "/") {
            let base = String(pattern[..<slash])
            let bits = String(pattern[pattern.index(after: slash)...])
            guard !bits.isEmpty, bits.count <= 3, bits.allSatisfy({ $0.isASCII && $0.isNumber }), let prefix = Int(bits), prefix <= 32 else { return false }
            let parts = base.split(separator: ".", omittingEmptySubsequences: false)
            guard !parts.isEmpty, parts.count <= 4 else { return false }
            var network: UInt32 = 0
            for index in 0..<4 {
                var octet: UInt32 = 0
                if index < parts.count {
                    let part = parts[index]
                    guard !part.isEmpty, part.count <= 3, part.allSatisfy({ $0.isASCII && $0.isNumber }), let value = UInt32(part), value <= 255 else { return false }
                    octet = value
                }
                network = (network << 8) | octet
            }
            guard let address = ipv4(host) else { return false }
            let mask: UInt32 = prefix == 0 ? 0 : (UInt32.max << UInt32(32 - prefix))
            return (address & mask) == (network & mask)
        }
        if pattern.hasPrefix("*.") { return host.hasSuffix(String(pattern.dropFirst())) }
        if pattern.hasPrefix(".") { return host == String(pattern.dropFirst()) || host.hasSuffix(pattern) }
        if pattern.contains("*") { return glob(Array(pattern), Array(host)) }
        return host == pattern
    }

    static func summarize(_ snapshot: ProxySnapshot) -> ProxySummary {
        guard snapshot.readable else { return ProxySummary(state: "unknown", kinds: [], campusBypass: "unknown") }
        var kinds: [String] = []
        if snapshot.http { kinds.append("http") }
        if snapshot.https { kinds.append("https") }
        if snapshot.socks { kinds.append("socks") }
        if snapshot.pac { kinds.append("pac") }
        if snapshot.autoDiscovery { kinds.append("autoDiscovery") }
        if kinds.isEmpty { return ProxySummary(state: "off", kinds: [], campusBypass: "notApplicable") }
        if !snapshot.http && !snapshot.https && !snapshot.socks { return ProxySummary(state: "on", kinds: kinds, campusBypass: "unknown") }
        let covered = campusHosts.filter { host in snapshot.exceptions.contains { patternMatches($0, host: host) } }.count
        return ProxySummary(state: "on", kinds: kinds, campusBypass: covered == campusHosts.count ? "all" : (covered > 0 ? "partial" : "none"))
    }

    /// macOS/iOS system proxy of the current network service (read-only).
    static func readSystem() -> ProxySnapshot {
        #if canImport(CFNetwork)
        guard let unmanaged = CFNetworkCopySystemProxySettings(),
              let settings = unmanaged.takeRetainedValue() as NSDictionary as? [String: Any] else { return ProxySnapshot() }
        func enabled(_ key: String) -> Bool { (settings[key] as? NSNumber)?.boolValue ?? false }
        var snapshot = ProxySnapshot(readable: true)
        snapshot.http = enabled("HTTPEnable")
        snapshot.https = enabled("HTTPSEnable")
        snapshot.socks = enabled("SOCKSEnable")
        snapshot.pac = enabled("ProxyAutoConfigEnable")
        snapshot.autoDiscovery = enabled("ProxyAutoDiscoveryEnable")
        snapshot.exceptions = settings["ExceptionsList"] as? [String] ?? []
        return snapshot
        #else
        return ProxySnapshot()
        #endif
    }
}

struct CompatInput: Equatable {
    var proxy = "unknown"          // on | off | unknown
    var tunnel = "unknown"         // detected | notDetected | unknown
    var campusDirect = "notRun"
    var campusSystem = "notRun"
    var publicDirect = "notRun"
    var publicSystem = "notRun"
}

struct CompatVerdict: Equatable {
    var verdict: String
    var recommendations: [String]
    var offerRules: Bool
}

enum CompatDiagnosis {
    static func diagnose(_ i: CompatInput) -> CompatVerdict {
        let r = "reachable"
        if [i.campusDirect, i.publicDirect, i.campusSystem, i.publicSystem].contains("cancelled") {
            return CompatVerdict(verdict: "cancelled", recommendations: [], offerRules: false)
        }
        if i.campusDirect == "tls" { return CompatVerdict(verdict: "campusTLS", recommendations: ["checkClock", "checkInterception"], offerRules: false) }
        if i.campusDirect == r && i.publicDirect == r { return CompatVerdict(verdict: "allReachable", recommendations: ["nothingToFix"], offerRules: false) }
        if i.campusDirect == r {
            if i.publicSystem == r { return CompatVerdict(verdict: "publicOnlyViaProxy", recommendations: ["loginFirst", "proxyMayHideLogin"], offerRules: false) }
            return CompatVerdict(verdict: "loginLikelyNeeded", recommendations: ["loginFirst"], offerRules: false)
        }
        if i.tunnel == "detected" {
            return CompatVerdict(verdict: "campusBlockedByTunnel", recommendations: ["addDirectRules", "tunBypassCampus", "retryAfterRules"], offerRules: true)
        }
        if i.campusSystem == r { return CompatVerdict(verdict: "campusOnlyViaProxy", recommendations: ["addDirectRules", "checkRouting"], offerRules: true) }
        if i.proxy == "on" {
            return CompatVerdict(verdict: "campusUnreachableWithProxy", recommendations: ["confirmCampusNetwork", "addDirectRules"], offerRules: true)
        }
        var verdict = CompatVerdict(verdict: i.publicDirect == r ? "notOnCampusNetwork" : "campusUnreachable",
                                    recommendations: ["confirmCampusNetwork"], offerRules: false)
        if verdict.verdict == "campusUnreachable" { verdict.recommendations.append("checkCable") }
        if i.tunnel == "unknown" { verdict.recommendations.append("tunnelStateUnknown") }
        return verdict
    }
}

/// Reuse the audited header-only transport: no cookies, credential store, redirects or client certificates.
enum HeaderCheck {
    static func resultToken(_ state: WebsiteProbeState) -> String {
        switch state {
        case .accessible: return "reachable"
        case .redirected: return "redirect"
        case .authenticationRequired: return "authentication"
        case .httpStatus: return "httpStatus"
        case .securityPolicy: return "securityPolicy"
        case .invalidInput, .network: return "network"
        default: return state.rawValue
        }
    }
    static func token(_ url: URL, followSystemProxy: Bool, checker: WebsiteProbe = WebsiteProbe()) async -> String {
        let result = await checker.probe(url.absoluteString, strategy: followSystemProxy ? .system : .direct)
        return resultToken(result.state)
    }
    static func compatibility(proxy: String, tunnel: String) async -> CompatInput {
        let campus = URL(string: "https://net.szu.edu.cn/srun_portal_pc")!
        let pub = URL(string: "https://www.apple.com/library/test/success.html")!
        var input = CompatInput(proxy: proxy, tunnel: tunnel)
        async let campusDirect = token(campus, followSystemProxy: false)
        async let publicDirect = token(pub, followSystemProxy: false)
        if proxy != "off" {
            async let campusSystem = token(campus, followSystemProxy: true)
            async let publicSystem = token(pub, followSystemProxy: true)
            (input.campusSystem, input.publicSystem) = await (campusSystem, publicSystem)
        }
        (input.campusDirect, input.publicDirect) = await (campusDirect, publicDirect)
        return input
    }
}

@MainActor final class CompatibilityRunner: ObservableObject {
    @Published private(set) var running = false
    @Published private(set) var input: CompatInput?
    @Published private(set) var verdict: CompatVerdict?
    private var task: Task<Void, Never>?
    private var generation = UUID()
    private let check: (String, String) async -> CompatInput
    init(check: @escaping (String, String) async -> CompatInput = { proxy, tunnel in
        await HeaderCheck.compatibility(proxy: proxy, tunnel: tunnel)
    }) { self.check = check }

    func run(proxy: String, tunnel: String, preview: Bool = false) {
        guard !running else { return }
        let id = UUID(); generation = id
        running = true
        input = nil
        verdict = nil
        task = Task { [weak self] in
            let result: CompatInput
            if preview {
                result = CompatInput(proxy: proxy, tunnel: tunnel, campusDirect: "notRun", campusSystem: "notRun", publicDirect: "notRun", publicSystem: "notRun")
            } else {
                result = await self?.check(proxy, tunnel) ?? CompatInput()
            }
            guard let self, self.generation == id else { return }
            var finished = result
            if Task.isCancelled {
                finished.campusDirect = "cancelled"
            }
            self.input = finished
            self.verdict = preview ? nil : CompatDiagnosis.diagnose(finished)
            self.running = false
        }
    }

    func cancel() { task?.cancel() }
    func reset() {
        generation = UUID()
        task?.cancel(); task = nil
        running = false; input = nil; verdict = nil
    }
}
