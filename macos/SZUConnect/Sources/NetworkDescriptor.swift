import Foundation

// Shared network-memory types, independent of system services for offline tests.
// Mirrors windows/SZUConnect/core/include/szu/netmemory.hpp.

enum MatchLevel: String, Sendable { case none, weak, medium, strong, conflict }
enum MemoryVerdict: String, Sendable { case notRemembered, conflict, awaitingProof, authorized }
enum RememberResult: Sendable { case remembered, needsProof, unreliable, readOnly, unreadable, nameConflict, failed }

/// What a remembered network still has to prove before a submission is allowed.
/// strong: the dormitory portal alone. medium, or strong while a tunnel / several interfaces make the physical
/// path ambiguous: HTTPS campus identity AND the dormitory portal. weak / none never authorize.
func memoryVerdict(level: MatchLevel, teachingConfirmed: Bool, dormitoryConfirmed: Bool, ambiguous: Bool) -> MemoryVerdict {
    if level == .conflict { return .conflict }
    guard level == .medium || level == .strong else { return .notRemembered }
    let bothNeeded = level == .medium || ambiguous
    let ok = bothNeeded ? (teachingConfirmed && dormitoryConfirmed) : dormitoryConfirmed
    return ok ? .authorized : .awaitingProof
}

/// Raw, RAM-only description of one live physical attachment. Labels: gw (gateway hardware id), gwip, net
/// (network address + prefix), dhcp, dns, sfx. Never displayed, logged, exported or stored raw.
struct NetworkDescriptor: Equatable, Sendable {
    var kind: String = "other"          // wifi | ethernet | other (label only, not identity)
    var raw: [String: String] = [:]

    init(kind: String = "other", raw: [String: String] = [:]) {
        self.kind = kind
        self.raw = raw
    }

    /// A hardware gateway id, or gateway address plus subnet.
    var isReliable: Bool {
        func has(_ key: String) -> Bool { !(raw[key] ?? "").isEmpty }
        return has("gw") || (has("gwip") && has("net"))
    }
}

/// Credential-free redirect classification only; never grants campus identity or login authority.
enum CampusPortalHint: String, Sendable {
    case teaching, dormitory
}

enum CampusRedirectTarget {
    /// Recognize only fixed portal pages as selection hints, never campus identity.
    /// The destination is never followed or retained, including HTTP and success pages.
    static func matches(_ location: String?, relativeTo url: URL) -> Bool {
        areaHint(location, relativeTo: url) != nil
    }

    /// A status/success page can select an entry, but does not request another login.
    static func authenticationRequired(_ location: String?, relativeTo url: URL) -> Bool {
        guard areaHint(location, relativeTo: url) != nil, let location,
              let target = URL(string: location, relativeTo: url)?.absoluteURL else { return false }
        return !["/a79.htm", "/srun_portal_success", "/srun_portal_success_mobile"].contains(target.path)
    }

    /// Only a typed selection hint survives parsing; no address, query or server data is kept.
    static func areaHint(_ location: String?, relativeTo url: URL) -> CampusPortalHint? {
        guard let location, !location.isEmpty, location.utf8.count <= 2048, !location.contains("\\"),
              !location.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }),
              let original = URLComponents(string: location),
              let originalPath = original.percentEncodedPath.removingPercentEncoding,
              !originalPath.split(separator: "/", omittingEmptySubsequences: false).contains(where: { $0 == "." || $0 == ".." }),
              let target = URL(string: location, relativeTo: url)?.absoluteURL,
              let parts = URLComponents(url: target, resolvingAgainstBaseURL: true),
              parts.user == nil, parts.password == nil, parts.fragment == nil,
              let scheme = parts.scheme?.lowercased(), let host = parts.host?.lowercased(),
              parts.percentEncodedHost?.contains("%") == false else { return nil }
        let port = parts.port ?? (scheme == "https" ? 443 : 80)
        switch (scheme, host, port) {
        case ("http", "net.szu.edu.cn", 80), ("https", "net.szu.edu.cn", 443):
            guard ["", "/", "/srun_portal_pc", "/srun_portal_phone", "/srun_portal_success", "/srun_portal_success_mobile", "/index_1.html"]
                .contains(parts.percentEncodedPath) else { return nil }
            return .teaching
        case ("http", "172.30.255.42", 801):
            guard ["", "/", "/eportal"].contains(parts.percentEncodedPath) else { return nil }
            return .dormitory
        case ("http", "172.30.255.42", 80):
            guard ["/a70.htm", "/a79.htm"].contains(parts.percentEncodedPath),
                  safeDormitoryQuery(parts.percentEncodedQuery) else { return nil }
            return .dormitory
        default: return nil
        }
    }

    private static func safeDormitoryQuery(_ query: String?) -> Bool {
        guard var text = query else { return true }
        for _ in 0..<4 {
            guard !text.contains("\\"),
                  !text.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else { return false }
            guard text.contains("%") else { return true }
            guard let decoded = text.removingPercentEncoding else { return false }
            text = decoded
        }
        return !text.contains("%") && !text.contains("\\")
            && !text.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) })
    }
}
