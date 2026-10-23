import Foundation

// No evaluation, networking or user configuration logging. Unknown JSON tokens remain verbatim.
enum ProxyAssistantFormat: String, CaseIterable, Identifiable {
    case clashScript, mihomo, singBox
    var id: String { rawValue }
}
struct ProxyAssistantFailure: Error, Equatable {
    let token: String
    init(_ token: String) { self.token = token }
}
struct ProxyAssistantPatch {
    let bytes: Data
    let changes: [String]
}

enum ProxyAssistantConfig {
    static let maximumBytes = 1_048_576
    static let domain = "net.szu.edu.cn"
    static let address = "172.30.255.42/32"
    static let rules = ["DOMAIN,net.szu.edu.cn,DIRECT", "IP-CIDR,172.30.255.42/32,DIRECT,no-resolve"]
    /// Append-only clipboard payload for v2rayN's routing rule editor, not a
    /// subscription or a complete client configuration. No local data is read.
    static let v2raynRules = """
    [
      {"remarks":"校园域名直连","enabled":true,"ruleType":1,"outboundTag":"direct","domain":["full:net.szu.edu.cn"]},
      {"remarks":"校园宿舍直连","enabled":true,"ruleType":1,"outboundTag":"direct","ip":["172.30.255.42/32"]}
    ]
    """
    static var script: String {
        // This script is copied or saved, never evaluated by SZUConnect.
        """
        // SZUConnect campus direct configuration v1
        function main(config, profileName) {
          const fail = (name) => { throw new Error("SZUConnect: " + name); };
          const object = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
          const list = (v) => v === undefined || (Array.isArray(v) && v.every((s) => typeof s === "string"));
          if (!object(config) || !list(config.rules)) fail("invalidConfig");
          if (config.mode !== undefined && config.mode !== "rule") fail("unsupportedMode");
          const prefix = ["DOMAIN,net.szu.edu.cn,DIRECT", "IP-CIDR,172.30.255.42/32,DIRECT,no-resolve"];
          const result = { ...config, rules: [...prefix, ...(config.rules || []).filter((r) => !prefix.includes(r))] };
          if (config.dns !== undefined) {
            if (!object(config.dns)) fail("invalidConfig");
            if (config.dns.enable !== undefined && typeof config.dns.enable !== "boolean") fail("unsupportedDns");
            if (config.dns.enable !== false && config.dns["enhanced-mode"] === "fake-ip") {
              const mode = config.dns["fake-ip-filter-mode"] === undefined ? "blacklist" : config.dns["fake-ip-filter-mode"];
              if (mode !== "blacklist" && mode !== "rule") fail("unsupportedDns");
              const previous = config.dns["fake-ip-filter"];
              if (!list(previous)) fail("unsupportedDns");
              const exact = mode === "rule" ? "DOMAIN,net.szu.edu.cn,real-ip" : "net.szu.edu.cn";
              result.dns = { ...config.dns, "fake-ip-filter": [exact, ...(previous || []).filter((s) => s !== exact)] };
            }
          }
          if (config.tun !== undefined) {
            if (!object(config.tun)) fail("invalidConfig");
            for (const key of ["enable", "auto-route"]) if (config.tun[key] !== undefined && typeof config.tun[key] !== "boolean") fail("unsupportedTun");
            if (config.tun.enable === true && config.tun["auto-route"] === true) {
              const previous = config.tun["route-exclude-address"];
              if (!list(previous)) fail("unsupportedTun");
              result.tun = { ...config.tun, "route-exclude-address": [...(previous || []).filter((s) => s !== "172.30.255.42/32"), "172.30.255.42/32"] };
            }
          }
          return result;
        }
        """ + "\n"
    }

    static func patch(_ data: Data, format: ProxyAssistantFormat) throws -> ProxyAssistantPatch {
        guard data.count <= maximumBytes else { throw ProxyAssistantFailure("tooLarge") }
        guard let decoded = String(data: data, encoding: .utf8), !decoded.contains("\0") else { throw ProxyAssistantFailure("encoding") }
        let text = decoded.hasPrefix("\u{FEFF}") ? String(decoded.dropFirst()) : decoded
        if format == .clashScript {
            if text == script { return ProxyAssistantPatch(bytes: data, changes: []) }
            guard defaultScript(text) else { throw ProxyAssistantFailure("unsupportedScript") }
            return ProxyAssistantPatch(bytes: Data(script.utf8), changes: ["rules", "dns", "tun"])
        }
        var root: ProxyNode
        if format == .mihomo && !text.trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("{") {
            root = try ProxyYAML.parse(text)
        } else {
            root = try ProxyJSON.parse(text)
        }
        guard root.members != nil else { throw ProxyAssistantFailure("invalidConfig") }
        let changes = try format == .mihomo ? patchMihomo(&root) : patchSingBox(&root)
        let output = changes.isEmpty ? data : Data((root.encoded + "\n").utf8)
        guard output.count <= maximumBytes else { throw ProxyAssistantFailure("tooLarge") }
        return ProxyAssistantPatch(bytes: output, changes: changes)
    }

    private static func defaultScript(_ text: String) -> Bool {
        let source = Array(text.unicodeScalars); var index = 0, tokens = [String](), afterReturn = false
        func newline(_ c: Unicode.Scalar) -> Bool { [10, 13, 0x2028, 0x2029].contains(c.value) }
        while index < source.count {
            let c = source[index]
            if newline(c) { if afterReturn { return false }; index += 1; continue }
            if c == " " || c == "\t" { index += 1; continue }
            if c == "/", index + 1 < source.count, source[index + 1] == "/" {
                index += 2; while index < source.count && !newline(source[index]) { index += 1 }; continue
            }
            if c == "/", index + 1 < source.count, source[index + 1] == "*" {
                index += 2; var closed = false
                while index + 1 < source.count {
                    if afterReturn && newline(source[index]) { return false }
                    if source[index] == "*" && source[index + 1] == "/" { index += 2; closed = true; break }; index += 1
                }
                if !closed { return false }; continue
            }
            let identifier = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_$")
            if identifier.contains(c) {
                let begin = index; index += 1
                while index < source.count && (identifier.contains(source[index]) || CharacterSet.decimalDigits.contains(source[index])) { index += 1 }
                let token = String(String.UnicodeScalarView(source[begin..<index])); tokens.append(token); afterReturn = token == "return"
            } else if "(){},;".unicodeScalars.contains(c) { tokens.append(String(c)); afterReturn = false; index += 1 }
            else { return false }
            if tokens.count > 16 { return false }
        }
        if tokens.isEmpty { return true }
        if tokens.last == ";" { tokens.removeLast() }
        if !tokens.contains(";") && !tokens.isEmpty { tokens.insert(";", at: tokens.count - 1) }
        return tokens == ["function", "main", "(", "config", ")", "{", "return", "config", ";", "}"] ||
            tokens == ["function", "main", "(", "config", ",", "profileName", ")", "{", "return", "config", ";", "}"]
    }

    private static func patchMihomo(_ root: inout ProxyNode) throws -> [String] {
        if let mode = root["mode"], mode.string != "rule" { throw ProxyAssistantFailure("unsupportedMode") }
        var changes = [String]()
        let old = try strings(root["rules"], error: "invalidConfig")
        let values = rules + old.filter { !rules.contains($0) }
        if values != old { root["rules"] = .strings(values); changes.append("rules") }
        if var dns = root["dns"] {
            guard dns.members != nil else { throw ProxyAssistantFailure("unsupportedDns") }
            try requireBool(dns["enable"], "unsupportedDns")
            if dns["enable"]?.literal != "false", dns["enhanced-mode"]?.string == "fake-ip" {
                if let filterMode = dns["fake-ip-filter-mode"], filterMode.string == nil { throw ProxyAssistantFailure("unsupportedDns") }
                let mode = dns["fake-ip-filter-mode"]?.string ?? "blacklist"
                // Whitelist semantics are opposite: adding the domain would force FakeIP. Reject it.
                guard ["blacklist", "rule"].contains(mode) else { throw ProxyAssistantFailure("unsupportedDns") }
                let previous = try strings(dns["fake-ip-filter"], error: "unsupportedDns")
                let exact = mode == "rule" ? "DOMAIN,net.szu.edu.cn,real-ip" : domain
                let next = [exact] + previous.filter { $0 != exact }
                if previous != next { dns["fake-ip-filter"] = .strings(next); root["dns"] = dns; changes.append("dns") }
            } else if let mode = dns["enhanced-mode"], mode.string == nil { throw ProxyAssistantFailure("unsupportedDns") }
        }
        if var tun = root["tun"] {
            guard tun.members != nil else { throw ProxyAssistantFailure("unsupportedTun") }
            try requireBool(tun["enable"], "unsupportedTun"); try requireBool(tun["auto-route"], "unsupportedTun")
            if tun["enable"]?.literal == "true", tun["auto-route"]?.literal == "true" {
                let previous = try strings(tun["route-exclude-address"], error: "unsupportedTun")
                if !previous.contains(address) { tun["route-exclude-address"] = .strings(previous + [address]); root["tun"] = tun; changes.append("tun") }
            }
        }
        return changes
    }

    private static func patchSingBox(_ root: inout ProxyNode) throws -> [String] {
        guard let outbounds = root["outbounds"]?.elements, outbounds.allSatisfy({ $0.members != nil }) else { throw ProxyAssistantFailure("unsupportedOutbound") }
        var tags = Set<String>(), direct: String?
        for outbound in outbounds {
            guard let type = outbound["type"]?.string else { throw ProxyAssistantFailure("unsupportedOutbound") }
            if let tag = outbound["tag"] {
                guard let value = tag.string, validTag(value), tags.insert(value).inserted else { throw ProxyAssistantFailure("unsupportedOutbound") }
            }
            if type == "direct" {
                guard direct == nil, let tag = outbound["tag"]?.string, validTag(tag), outbound["override_address"] == nil, outbound["override_port"] == nil,
                      outbound["detour"] == nil || outbound["detour"]?.string == "" else { throw ProxyAssistantFailure("unsupportedOutbound") }
                direct = tag
            }
        }
        guard let direct else { throw ProxyAssistantFailure("unsupportedOutbound") }
        var route = root["route"] ?? .object([])
        guard route.members != nil else { throw ProxyAssistantFailure("invalidConfig") }
        let oldRules = try objects(route["rules"], "invalidConfig")
        var modern = oldRules.contains { $0["action"] != nil }
        var dns = root["dns"]
        var dnsRules: [ProxyNode] = [], localTags = [String](), fakeIP = false
        if let value = dns {
            guard value.members != nil else { throw ProxyAssistantFailure("unsupportedDns") }
            dnsRules = try objects(value["rules"], "unsupportedDns")
            let servers = try objects(value["servers"], "unsupportedDns")
            var dnsTags = Set<String>(), typed = false, legacy = false
            for server in servers {
                let type = server["type"]?.string, address = server["address"]?.string
                guard (server["type"] == nil || type != nil), (server["address"] == nil || address != nil), (type == nil) != (address == nil) else { throw ProxyAssistantFailure("unsupportedDns") }
                if let tag = server["tag"] { guard let text = tag.string, validTag(text), dnsTags.insert(text).inserted else { throw ProxyAssistantFailure("unsupportedDns") } }
                if let type, !["local", "hosts", "tcp", "udp", "tls", "quic", "https", "h3", "dhcp", "mdns", "fakeip", "tailscale", "openconnect", "openvpn", "resolved"].contains(type) { throw ProxyAssistantFailure("unsupportedDns") }
                typed = typed || type != nil; legacy = legacy || address != nil
                fakeIP = fakeIP || type == "fakeip" || address == "fakeip"
                if type == "local" || type == "dhcp" || address == "local" || address?.hasPrefix("dhcp://") == true {
                    guard let tag = server["tag"]?.string, validTag(tag), server["detour"] == nil || server["detour"]?.string == "" else { throw ProxyAssistantFailure("unsupportedDns") }
                    localTags.append(tag)
                }
            }
            guard !(typed && legacy) else { throw ProxyAssistantFailure("unsupportedDns") }
            modern = modern || typed
            if let fake = value["fakeip"] { guard fake.members != nil else { throw ProxyAssistantFailure("unsupportedDns") }; try requireBool(fake["enabled"], "unsupportedDns"); fakeIP = fakeIP || fake["enabled"]?.literal == "true" }
            if fakeIP, localTags.count != 1 { throw ProxyAssistantFailure("unsupportedDns") }
        }
        var changes = [String]()
        func rule(_ key: String, _ target: String, _ outputKey: String, _ tag: String) -> ProxyNode {
            var fields: [(String, ProxyNode)] = [(key, .strings([target])), (outputKey, .text(tag))]
            if modern { fields.append(("action", .text("route"))) }
            return .object(fields)
        }
        let prefix = [rule("domain", domain, "outbound", direct), rule("ip_cidr", address, "outbound", direct)]
        func owned(_ node: ProxyNode, _ field: String, _ target: String, _ output: String, _ tag: String) -> Bool {
            guard let fields = node.members, (2...3).contains(fields.count), fields.allSatisfy({ [field, output, "action"].contains($0.0) }),
                  node[output]?.string == tag, node["action"] == nil || node["action"]?.string == "route" else { return false }
            return node[field]?.string == target || node[field]?.elements?.map(\.string) == [target]
        }
        let filtered = oldRules.filter { !owned($0, "domain", domain, "outbound", direct) && !owned($0, "ip_cidr", address, "outbound", direct) }
        let next = prefix + filtered
        if oldRules.map(\.encoded) != next.map(\.encoded) { route["rules"] = .array(next); root["route"] = route; changes.append("rules") }
        if fakeIP, let tag = localTags.first {
            let nextDNS = [rule("domain", domain, "server", tag)] + dnsRules.filter { !owned($0, "domain", domain, "server", tag) }
            if nextDNS.map(\.encoded) != dnsRules.map(\.encoded) { dns?["rules"] = .array(nextDNS); root["dns"] = dns; changes.append("dns") }
        }
        if let inbounds = root["inbounds"] {
            var values = try objects(inbounds, "invalidConfig"), didChange = false
            for index in values.indices where values[index]["type"]?.string == "tun" {
                try requireBool(values[index]["auto_route"], "unsupportedTun"); try requireBool(values[index]["strict_route"], "unsupportedTun")
                if values[index]["auto_route"]?.literal == "true" {
                    guard values[index]["inet4_route_exclude_address"] == nil, values[index]["inet6_route_exclude_address"] == nil,
                          modern || values[index]["route_exclude_address"] != nil else { throw ProxyAssistantFailure("unsupportedTun") }
                    let previous = try strings(values[index]["route_exclude_address"], error: "unsupportedTun")
                    if !previous.contains(address) { values[index]["route_exclude_address"] = .strings(previous + [address]); didChange = true }
                }
            }
            if didChange { root["inbounds"] = .array(values); changes.append("tun") }
        }
        return changes
    }
    private static func validTag(_ value: String) -> Bool { !value.isEmpty && value.utf8.count <= 256 && !value.unicodeScalars.contains { $0.value < 32 || $0.value == 127 } }
    private static func requireBool(_ value: ProxyNode?, _ error: String) throws { if let value, !["true", "false"].contains(value.literal ?? "") { throw ProxyAssistantFailure(error) } }
    private static func strings(_ value: ProxyNode?, error: String) throws -> [String] {
        guard let value else { return [] }
        guard let list = value.elements, list.allSatisfy({ $0.string != nil }) else { throw ProxyAssistantFailure(error) }
        return list.compactMap(\.string)
    }
    private static func objects(_ value: ProxyNode?, _ error: String) throws -> [ProxyNode] {
        guard let value else { return [] }
        guard let list = value.elements, list.allSatisfy({ $0.members != nil }) else { throw ProxyAssistantFailure(error) }
        return list
    }
}

private struct ProxyNode {
    var raw: String?
    var members: [(String, ProxyNode)]?
    var elements: [ProxyNode]?
    var string: String?
    var literal: String?
    var encoded: String {
        if let raw { return raw }
        if let members { return "{" + members.map { Self.quote($0.0) + ":" + $0.1.encoded }.joined(separator: ",") + "}" }
        if let elements { return "[" + elements.map(\.encoded).joined(separator: ",") + "]" }
        return string.map(Self.quote) ?? literal ?? "null"
    }
    subscript(_ key: String) -> ProxyNode? {
        get { members?.first { $0.0 == key }?.1 }
        set { raw = nil; if let index = members?.firstIndex(where: { $0.0 == key }) { if let newValue { members?[index].1 = newValue } else { members?.remove(at: index) } } else if let newValue { members?.append((key, newValue)) } }
    }
    static func quote(_ value: String) -> String { String(data: try! JSONEncoder().encode(value), encoding: .utf8)! }
    static func text(_ value: String) -> Self { Self(string: value) }
    static func strings(_ values: [String]) -> Self { .array(values.map(Self.text)) }
    static func array(_ values: [Self]) -> Self { Self(elements: values) }
    static func object(_ fields: [(String, Self)]) -> Self { Self(members: fields) }
}

private struct ProxyJSON {
    let bytes: [UInt8]
    var index = 0, nodes = 0
    static func parse(_ text: String) throws -> ProxyNode {
        var parser = Self(bytes: Array(text.utf8)); let root = try parser.value(0); parser.space()
        guard parser.index == parser.bytes.count else { throw ProxyAssistantFailure("invalidConfig") }; return root
    }
    mutating func space() { while index < bytes.count && [9, 10, 13, 32].contains(bytes[index]) { index += 1 } }
    mutating func quoted() throws -> String {
        let begin = index; index += 1; var escaped = false
        while index < bytes.count {
            let byte = bytes[index]; index += 1
            if escaped { escaped = false; continue }
            if byte == 92 { escaped = true; continue }
            if byte == 34 { guard let result = try? JSONDecoder().decode(String.self, from: Data(bytes[begin..<index])) else { break }; return result }
            if byte < 32 { break }
        }
        throw ProxyAssistantFailure("invalidConfig")
    }
    mutating func value(_ depth: Int) throws -> ProxyNode {
        space(); nodes += 1
        guard depth <= 48, nodes <= 32768, index < bytes.count else { throw ProxyAssistantFailure("invalidConfig") }
        let begin = index; var node: ProxyNode
        if bytes[index] == 34 { node = .text(try quoted()) }
        else if bytes[index] == 123 || bytes[index] == 91 {
            let object = bytes[index] == 123, close: UInt8 = object ? 125 : 93
            index += 1; space(); var fields = [(String, ProxyNode)](), values = [ProxyNode](), keys = Set<String>()
            if index < bytes.count && bytes[index] == close { index += 1 }
            else {
                while true {
                    if object {
                        guard index < bytes.count, bytes[index] == 34 else { throw ProxyAssistantFailure("invalidConfig") }
                        let key = try quoted(); guard keys.insert(key).inserted else { throw ProxyAssistantFailure("invalidConfig") }
                        space(); guard index < bytes.count, bytes[index] == 58 else { throw ProxyAssistantFailure("invalidConfig") }; index += 1
                        fields.append((key, try value(depth + 1)))
                    } else { values.append(try value(depth + 1)) }
                    space(); guard index < bytes.count else { throw ProxyAssistantFailure("invalidConfig") }
                    if bytes[index] == close { index += 1; break }
                    guard bytes[index] == 44 else { throw ProxyAssistantFailure("invalidConfig") }; index += 1; space()
                }
            }
            node = object ? .object(fields) : .array(values)
        } else {
            while index < bytes.count && ![9, 10, 13, 32, 44, 93, 125].contains(bytes[index]) { index += 1 }
            let token = String(decoding: bytes[begin..<index], as: UTF8.self)
            guard ["true", "false", "null"].contains(token) || token.range(of: #"^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$"#, options: .regularExpression) != nil else { throw ProxyAssistantFailure("invalidConfig") }
            node = ProxyNode(literal: token)
        }
        node.raw = String(decoding: bytes[begin..<index], as: UTF8.self); return node
    }
}

// A deliberately bounded YAML subset. Unsupported tags/anchors/multiline/flow YAML are rejected,
// never guessed. Output is JSON, a YAML 1.2 subset, so scalar types and huge integer tokens survive.
private struct ProxyYAML {
    struct Line { let indent: Int; let text: String }
    var lines: [Line]; var index = 0, nodes = 0
    static func parse(_ text: String) throws -> ProxyNode {
        var lines = [Line]()
        for raw in text.components(separatedBy: .newlines) {
            guard !raw.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else { throw ProxyAssistantFailure("invalidConfig") }
            var result = "", single = false, double = false, escaped = false
            for c in raw {
                if escaped { result.append(c); escaped = false; continue }
                if c == "\\" && double { escaped = true; result.append(c); continue }
                if c == "\"" && !single { double.toggle() }
                if c == "'" && !double { single.toggle() }
                if c == "#" && !single && !double && (result.isEmpty || result.last == " ") { break }
                result.append(c)
            }
            let content = result.trimmingCharacters(in: CharacterSet(charactersIn: " "))
            if content.isEmpty { continue }
            guard !content.hasPrefix("---"), !content.hasPrefix("..."), !content.hasPrefix("%"), !single, !double else { throw ProxyAssistantFailure("invalidConfig") }
            lines.append(Line(indent: result.prefix { $0 == " " }.count, text: content))
        }
        guard let first = lines.first, first.indent == 0 else { throw ProxyAssistantFailure("invalidConfig") }
        var parser = Self(lines: lines); let node = try parser.block(0, depth: 0)
        guard parser.index == parser.lines.count, node.members != nil else { throw ProxyAssistantFailure("invalidConfig") }; return node
    }
    mutating func block(_ indent: Int, depth: Int) throws -> ProxyNode {
        guard depth <= 48 else { throw ProxyAssistantFailure("invalidConfig") }
        let sequence = lines[index].text.hasPrefix("- ")
        var fields = [(String, ProxyNode)](), values = [ProxyNode](), keys = Set<String>()
        while index < lines.count && lines[index].indent == indent {
            nodes += 1; guard nodes <= 32768 else { throw ProxyAssistantFailure("invalidConfig") }
            let text = lines[index].text
            if sequence {
                guard text.hasPrefix("- ") else { throw ProxyAssistantFailure("invalidConfig") }
                let tail = String(text.dropFirst(2)); index += 1
                if !tail.hasPrefix("\"") && !tail.hasPrefix("'") && !tail.hasPrefix("{") && !tail.hasPrefix("[") && tail.contains(": ") {
                    // A sequence mapping continues at the indentation following its '- '.
                    lines.insert(Line(indent: indent + 2, text: tail), at: index)
                    values.append(try block(indent + 2, depth: depth + 1))
                } else { values.append(try scalar(tail)) }
            } else {
                guard let split = text.firstIndex(of: ":"), text.index(after: split) == text.endIndex || text[text.index(after: split)] == " " else { throw ProxyAssistantFailure("invalidConfig") }
                let keyRaw = String(text[..<split]), keyNode = try scalar(keyRaw)
                guard let key = keyNode.string, !key.isEmpty, key != "<<", keys.insert(key).inserted else { throw ProxyAssistantFailure("invalidConfig") }
                let tail = String(text[text.index(after: split)...]).trimmingCharacters(in: CharacterSet(charactersIn: " ")); index += 1
                let node: ProxyNode
                if tail.isEmpty {
                    guard index < lines.count, lines[index].indent > indent else { throw ProxyAssistantFailure("invalidConfig") }
                    node = try block(lines[index].indent, depth: depth + 1)
                } else { node = try scalar(tail) }
                fields.append((key, node))
            }
            if index < lines.count && lines[index].indent > indent { throw ProxyAssistantFailure("invalidConfig") }
        }
        return sequence ? .array(values) : .object(fields)
    }
    func scalar(_ text: String) throws -> ProxyNode {
        guard !text.isEmpty, !"!&*|>@`".contains(text.first!), !text.contains("\0") else { throw ProxyAssistantFailure("invalidConfig") }
        if text.hasPrefix("\"") || text.hasPrefix("{") { return try ProxyJSON.parse(text) }
        if text.hasPrefix("[") {
            if let json = try? ProxyJSON.parse(text) { return json }
            guard text.hasSuffix("]") else { throw ProxyAssistantFailure("invalidConfig") }
            let inside = String(text.dropFirst().dropLast()); var pieces = [String](), current = "", single = false, double = false, escaped = false
            for c in inside {
                if escaped { current.append(c); escaped = false; continue }
                if c == "\\" && double { current.append(c); escaped = true; continue }
                if c == "\"" && !single { double.toggle() }
                if c == "'" && !double { single.toggle() }
                if !single && !double {
                    if "[]{}".contains(c) { throw ProxyAssistantFailure("invalidConfig") }
                    if c == "," { pieces.append(current.trimmingCharacters(in: CharacterSet(charactersIn: " "))); current = ""; continue }
                }
                current.append(c)
            }
            guard !single, !double, pieces.count < 16384 else { throw ProxyAssistantFailure("invalidConfig") }
            pieces.append(current.trimmingCharacters(in: CharacterSet(charactersIn: " ")))
            return .array(try pieces.map(scalar))
        }
        if text.hasPrefix("'") {
            guard text.hasSuffix("'"), text.count >= 2 else { throw ProxyAssistantFailure("invalidConfig") }
            let inner = Array(text.dropFirst().dropLast()); var offset = 0, value = ""
            while offset < inner.count {
                let character = inner[offset]; offset += 1
                if character == "'" {
                    guard offset < inner.count, inner[offset] == "'" else { throw ProxyAssistantFailure("invalidConfig") }
                    offset += 1
                }
                value.append(character)
            }
            return .text(value)
        }
        if ["true", "false", "null"].contains(text) || text.range(of: #"^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$"#, options: .regularExpression) != nil { return try ProxyJSON.parse(text) }
        guard !text.contains(": "), !text.contains("["), !text.contains("]"), !text.contains("{"), !text.contains("}"),
              !text.contains("\""), !text.contains("'"), !["-", "?", ":"].contains(text),
              !["- ", "? ", ": "].contains(where: text.hasPrefix),
              !["yes", "no", "on", "off", "~", ".nan", ".inf", "-.inf", "true", "false", "null"].contains(text.lowercased()),
              (text.range(of: #"^[+-]?(?:[0-9]|\.[0-9])"#, options: .regularExpression) == nil || text.range(of: #"^[0-9]{1,3}(\.[0-9]{1,3}){3}(/[0-9]{1,2})?$"#, options: .regularExpression) != nil) else { throw ProxyAssistantFailure("invalidConfig") }
        return .text(text)
    }
}
