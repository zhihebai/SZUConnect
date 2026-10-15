import Foundation
import CryptoKit
#if os(macOS)
import Darwin
import SystemConfiguration
import SystemConfiguration.SCDynamicStoreCopyDHCPInfo
#endif

/// Live physical-network evidence, used only in memory. Never persist or log its fields.
struct CampusPhysicalRoute: Equatable, Sendable {
    let interfaceName: String
    let interfaceIndex: UInt32
    let addresses: [String]
    let dnsServers: [String]
    let identity: String
    var serviceID: String = ""
    var descriptor: NetworkDescriptor? = nil
    /// Current selection preference only. A system-primary change cannot revoke a socket
    /// already bound to this independently proven physical interface and source address.
    var isSystemPrimary: Bool = false

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.interfaceName == rhs.interfaceName && lhs.interfaceIndex == rhs.interfaceIndex
            && lhs.addresses == rhs.addresses && lhs.dnsServers == rhs.dnsServers
            && lhs.identity == rhs.identity && lhs.serviceID == rhs.serviceID
            && lhs.descriptor == rhs.descriptor
    }
}

enum CampusPhysicalSelection: Equatable, Sendable {
    case system
    case bound(CampusPhysicalRoute)
    case unavailable
}

enum CampusPhysicalNetwork {
    #if os(macOS)
    struct Service: Equatable, Sendable {
        let interface: String
        let router: String
        let addresses: [String]
        let masks: [String]
        let linkActive: Bool
        let dhcpDNS: [String]
        let serviceDNS: [String]
        /// Complete associated IPv4, link, DHCP-DNS and service-DNS records.
        let evidence: String
        var descriptor: NetworkDescriptor? = nil
    }
    struct Evidence: Equatable, Sendable {
        let primaryService: String?
        let primaryInterface: String?
        let indices: [String: UInt32]
        let liveAddresses: [String: [String]]
        let services: [String: Service]
        let routes: TunnelRouteSnapshot
        let globalEvidence: String
    }

    /// An implicit TUN selection requires the unscoped default to agree with the primary.
    /// An explicit remembered interface instead proves its own bound default and descriptor.
    static func select(_ evidence: Evidence, interfaceName: String? = nil) -> CampusPhysicalSelection {
        if let interfaceName {
            let matches = candidates(evidence).filter { $0.interfaceName == interfaceName }
            guard matches.count == 1, let route = matches.first else { return .unavailable }
            return .bound(route)
        }
        guard evidence.routes.hasTunnelRoutes else { return .system }
        guard let id = evidence.primaryService, let name = evidence.primaryInterface,
              let service = evidence.services[id], service.interface == name,
              validService(service, in: evidence),
              let index = evidence.indices[name] else { return .unavailable }
        let defaults = Set(evidence.routes.physicalDefaults)
        guard defaults.count == 1, defaults.first?.interface == name,
              defaults.first?.gateway == service.router else { return .unavailable }
        guard let route = boundRoute(id: id, service: service, index: index, evidence: evidence) else { return .unavailable }
        return .bound(route)
    }

    private static func validService(_ service: Service, in evidence: Evidence) -> Bool {
        let name = service.interface
        guard service.linkActive, let index = evidence.indices[name], index > 0,
              let live = evidence.liveAddresses[name], !live.isEmpty,
              !service.addresses.isEmpty, service.addresses.allSatisfy(CampusPhysicalDNS.isUsableIPv4),
              Set(service.addresses).isSubset(of: Set(live)),
              service.masks.count == 1 || service.masks.count == service.addresses.count,
              service.addresses.enumerated().allSatisfy({ offset, address in
                  NetworkContinuitySnapshot.networkText(address: address,
                      mask: service.masks[service.masks.count == 1 ? 0 : offset]) != nil
              }), CampusPhysicalDNS.isUsableIPv4(service.router) else { return false }
        // DHCP option 6 is interface-associated and cannot be replaced by global TUN DNS.
        // A configured service-specific resolver is the only fallback for static networks.
        let dns = service.dhcpDNS.isEmpty ? service.serviceDNS : service.dhcpDNS
        return !dns.isEmpty && dns.count <= 8 && dns.allSatisfy(CampusPhysicalDNS.isUsableIPv4)
    }

    private static func boundRoute(id: String, service: Service, index: UInt32, evidence: Evidence) -> CampusPhysicalRoute? {
        let dns = service.dhcpDNS.isEmpty ? service.serviceDNS : service.dhcpDNS
        // Socket authority comes from this service's current attachment, not the TUN's
        // routes or another NIC's metadata. Those can change while the bound source is
        // unchanged. Include every selected-service record and bound default so changes
        // to the actual source, gateway, resolver or network descriptor still revoke it.
        let defaults = Set(evidence.routes.boundPhysicalDefaults.filter { $0.interface == service.interface })
            .sorted { ($0.interface, $0.gateway) < ($1.interface, $1.gateway) }
        let object: [String: Any] = ["selected": [id, service.interface], "index": index,
            "liveAddresses": evidence.liveAddresses[service.interface] ?? [],
            "router": service.router, "addresses": service.addresses, "masks": service.masks,
            "linkActive": service.linkActive, "dhcpDNS": service.dhcpDNS, "serviceDNS": service.serviceDNS,
            "serviceEvidence": service.evidence,
            "descriptor": ["kind": service.descriptor?.kind ?? "", "raw": service.descriptor?.raw ?? [:]],
            "boundPhysicalDefaults": defaults.map { [$0.interface, $0.gateway] }]
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return nil }
        let identity = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        // The descriptor's subnet comes from the associated IPv4 record's first address.
        // DNS and HTTP sockets must bind that same source, not the lexicographically first alias.
        guard let source = service.addresses.first else { return nil }
        let addresses = [source] + Set(service.addresses).subtracting([source]).sorted()
        return .init(interfaceName: service.interface, interfaceIndex: index,
            addresses: addresses, dnsServers: Array(Set(dns)).sorted(), identity: identity,
            serviceID: id, descriptor: service.descriptor,
            isSystemPrimary: evidence.primaryService == id && evidence.primaryInterface == service.interface)
    }

    /// Each candidate proves only its own bound route, address and associated resolver.
    /// Selection among remembered networks happens separately, without changing OS routes.
    static func candidates(_ evidence: Evidence) -> [CampusPhysicalRoute] {
        let serviceCounts = Dictionary(grouping: evidence.services.values, by: \.interface).mapValues(\.count)
        return evidence.services.keys.sorted().compactMap { id -> CampusPhysicalRoute? in
            guard let service = evidence.services[id], serviceCounts[service.interface] == 1,
                  let descriptor = service.descriptor, descriptor.isReliable,
                  descriptor.kind == "wifi" || descriptor.kind == "ethernet",
                  validService(service, in: evidence), let index = evidence.indices[service.interface] else { return nil }
            let defaults = Set(evidence.routes.boundPhysicalDefaults.filter { $0.interface == service.interface })
            guard defaults.count == 1, defaults.first?.gateway == service.router else { return nil }
            return boundRoute(id: id, service: service, index: index, evidence: evidence)
        }.sorted {
            if $0.isSystemPrimary != $1.isSystemPrimary { return $0.isSystemPrimary }
            return ($0.interfaceName, $0.serviceID) < ($1.interfaceName, $1.serviceID)
        }
    }

    static func capture(interfaceName: String? = nil) -> CampusPhysicalSelection {
        guard let store = SCDynamicStoreCreate(nil, "local.SZUConnect.physicalTransport" as CFString, nil, nil),
              let first = read(store) else { return .unavailable }
        // A selected physical attachment must be independently proven in both reads.
        // Explicit interface-bound requests do not inherit an unrelated TUN transition.
        guard let second = read(store) else { return .unavailable }
        return selectStable(first, second, interfaceName: interfaceName)
    }

    static func selectStable(_ first: Evidence, _ second: Evidence, interfaceName: String? = nil) -> CampusPhysicalSelection {
        if let interfaceName {
            let matches = candidatesStable(first, second).filter { $0.interfaceName == interfaceName }
            guard matches.count == 1, let route = matches.first else { return .unavailable }
            return .bound(route)
        }
        // Legacy implicit system transport has no bound source. Keep its complete
        // snapshot gate, including TUN starting/stopping during the collection.
        guard first.routes.hasTunnelRoutes == second.routes.hasTunnelRoutes else { return .unavailable }
        if !second.routes.hasTunnelRoutes {
            return first == second ? .system : .unavailable
        }
        let previous = select(first), current = select(second)
        return previous == current ? current : .unavailable
    }

    static func candidates() -> [CampusPhysicalRoute] {
        guard let store = SCDynamicStoreCreate(nil, "local.SZUConnect.physicalCandidates" as CFString, nil, nil),
              let first = read(store), let second = read(store) else { return [] }
        return candidatesStable(first, second)
    }

    static func candidatesStable(_ first: Evidence, _ second: Evidence) -> [CampusPhysicalRoute] {
        let previous = candidates(first)
        // Each returned candidate has its own consistent proof in both samples. A
        // changing/missing NIC cannot revoke another interface's unchanged attachment,
        // and a newly appearing interface cannot acquire authority from one sample.
        return candidates(second).filter { previous.contains($0) }
    }

    private static func read(_ store: SCDynamicStore) -> Evidence? {
        var indices: [String: UInt32] = [:]
        var kinds: [String: String] = [:]
        for interface in (SCNetworkInterfaceCopyAll() as? [SCNetworkInterface]) ?? [] {
            let type = SCNetworkInterfaceGetInterfaceType(interface) as String?
            guard type == kSCNetworkInterfaceTypeIEEE80211 as String || type == kSCNetworkInterfaceTypeEthernet as String,
                  let name = SCNetworkInterfaceGetBSDName(interface) as String? else { continue }
            let index = if_nametoindex(name)
            if index > 0 {
                indices[name] = index
                kinds[name] = type == kSCNetworkInterfaceTypeIEEE80211 as String ? "wifi" : "ethernet"
            }
        }
        guard let routes = TunnelRouteSnapshot.capture(physicalInterfaces: Set(indices.keys)) else { return nil }
        // IPv6-only ordinary networks may publish neither IPv4 global nor service state.
        // Two coherent route reads can preserve system transport without granting any bound
        // IPv4 authority. Under TUN the missing primary/service fields still fail selection.
        let global = SCDynamicStoreCopyValue(store, "State:/Network/Global/IPv4" as CFString) as? [String: Any] ?? [:]
        let raw = SCDynamicStoreCopyMultiple(store, nil,
            ["State:/Network/Service/.*/IPv4"] as CFArray) as? [String: Any] ?? [:]
        var live: [String: [String]] = [:]
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0 else { return nil }
        defer { freeifaddrs(list) }
        var node = list
        while let current = node {
            let item = current.pointee
            if let pointer = item.ifa_name, let address = item.ifa_addr,
               address.pointee.sa_family == AF_INET,
               item.ifa_flags & UInt32(IFF_UP | IFF_RUNNING) == UInt32(IFF_UP | IFF_RUNNING) {
                let name = String(cString: pointer)
                if indices[name] != nil {
                    let ipv4 = UnsafeRawPointer(address).assumingMemoryBound(to: sockaddr_in.self).pointee
                    var value = ipv4.sin_addr
                    var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                    if inet_ntop(AF_INET, &value, &buffer, socklen_t(buffer.count)) != nil {
                        live[name, default: []].append(String(cString: buffer))
                    }
                }
            }
            node = item.ifa_next
        }
        live = live.mapValues { Array(Set($0)).sorted() }
        var services: [String: Service] = [:]
        for (key, value) in raw {
            let parts = key.split(separator: "/")
            guard parts.count == 5, let ipv4 = value as? [String: Any],
                  let name = ipv4["InterfaceName"] as? String, indices[name] != nil else { continue }
            let id = String(parts[3])
            let link = SCDynamicStoreCopyValue(store, "State:/Network/Interface/\(name)/Link" as CFString) as? [String: Any] ?? [:]
            let dns = SCDynamicStoreCopyValue(store, "State:/Network/Service/\(id)/DNS" as CFString) as? [String: Any] ?? [:]
            let dhcp = SCDynamicStoreCopyDHCPInfo(store, id as CFString)
            let option = dhcp.flatMap { DHCPInfoGetOptionData($0, 6) as Data? }
            let serverOption = dhcp.flatMap { DHCPInfoGetOptionData($0, 54) as Data? }
            let server: String?
            if let serverOption {
                guard serverOption.count == 4 else { return nil }
                let address = serverOption.map(String.init).joined(separator: ".")
                guard CampusPhysicalDNS.isUsableIPv4(address) else { return nil }
                server = address
            } else { server = nil }
            // A present malformed DHCP option must not silently use another resolver.
            let dhcpDNS: [String]
            if let option {
                guard let decoded = decodeDNSOption(option) else { return nil }
                dhcpDNS = decoded
            } else { dhcpDNS = [] }
            let descriptor = NetworkContinuitySnapshot.makeDescriptor(kind: kinds[name] ?? "", ipv4: ipv4,
                dhcpServer: server, dnsServers: dns["ServerAddresses"] as? [String] ?? [],
                searchDomain: (dns["DomainName"] as? String) ?? (dns["SearchDomains"] as? [String])?.first)
            guard let canonical = canonicalRecord(["IPv4": ipv4, "Link": link, "DNS": dns,
                "DHCP-DNS": option?.base64EncodedString() ?? "",
                "DHCP-Server": serverOption?.base64EncodedString() ?? "",
                "Descriptor": ["kind": descriptor.kind, "raw": descriptor.raw]]) else { return nil }
            services[id] = Service(interface: name, router: ipv4["Router"] as? String ?? "",
                addresses: ipv4["Addresses"] as? [String] ?? [], masks: ipv4["SubnetMasks"] as? [String] ?? [],
                linkActive: link["Active"] as? Bool == true, dhcpDNS: dhcpDNS,
                serviceDNS: dns["ServerAddresses"] as? [String] ?? [], evidence: canonical, descriptor: descriptor)
        }
        guard let globalRecord = canonicalRecord(global) else { return nil }
        return Evidence(primaryService: global["PrimaryService"] as? String,
            primaryInterface: global["PrimaryInterface"] as? String, indices: indices,
            liveAddresses: live, services: services, routes: routes, globalEvidence: globalRecord)
    }

    static func decodeDNSOption(_ data: Data) -> [String]? {
        guard !data.isEmpty, data.count % 4 == 0, data.count <= 32 else { return nil }
        let bytes = [UInt8](data)
        let values = stride(from: 0, to: bytes.count, by: 4).map { offset in
            bytes[offset..<(offset + 4)].map(String.init).joined(separator: ".")
        }
        return values.allSatisfy(CampusPhysicalDNS.isUsableIPv4) ? values : nil
    }

    private static func canonicalRecord(_ object: [String: Any]) -> String? {
        func normalize(_ value: Any) -> Any {
            if let data = value as? Data { return ["data": data.base64EncodedString()] }
            if let date = value as? Date { return ["date": date.timeIntervalSince1970] }
            if let dictionary = value as? [String: Any] { return dictionary.mapValues(normalize) }
            if let array = value as? [Any] { return array.map(normalize) }
            return value
        }
        guard let data = try? JSONSerialization.data(withJSONObject: normalize(object), options: [.sortedKeys]) else { return nil }
        return String(data: data, encoding: .utf8)
    }
    #else
    static func capture(interfaceName: String? = nil) -> CampusPhysicalSelection { interfaceName == nil ? .system : .unavailable }
    static func candidates() -> [CampusPhysicalRoute] { [] }
    #endif
}

enum CampusPhysicalDNSError: Error, Equatable {
    case cancelled, timeout, unavailable, invalidResponse
}

/// A deliberately small IPv4 resolver for the credential-free campus discovery/probe hosts.
/// It never uses getaddrinfo, a global DNS cache, a public fallback resolver or a TUN resolver.
enum CampusPhysicalDNS {
    static let allowedHosts: Set<String> = ["net.szu.edu.cn", "www.apple.com", "www.msftconnecttest.com"]
    static func isUsableIPv4(_ value: String) -> Bool {
        let parts = value.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        let bytes = parts.compactMap { part -> UInt8? in
            guard !part.isEmpty, part.count <= 3, part.allSatisfy({ $0.isASCII && $0.isNumber }),
                  !(part.count > 1 && part.first == "0") else { return nil }
            return UInt8(part)
        }
        guard bytes.count == 4 else { return false }
        return bytes[0] != 0 && bytes[0] != 127 && bytes[0] < 224
            && !(bytes[0] == 169 && bytes[1] == 254)
            && !(bytes[0] == 198 && (bytes[1] == 18 || bytes[1] == 19))
    }

    static func query(host: String, transaction: UInt16) throws -> Data {
        guard allowedHosts.contains(host) else { throw CampusPhysicalDNSError.unavailable }
        var bytes: [UInt8] = [UInt8(transaction >> 8), UInt8(transaction & 255), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0]
        for label in host.split(separator: ".") {
            bytes.append(UInt8(label.utf8.count)); bytes += label.utf8
        }
        bytes += [0, 0, 1, 0, 1]
        return Data(bytes)
    }

    static func parse(_ data: Data, host: String, transaction: UInt16) throws -> [String] {
        guard allowedHosts.contains(host), data.count >= 12, data.count <= 65_535 else { throw CampusPhysicalDNSError.invalidResponse }
        let bytes = [UInt8](data)
        func u16(_ offset: Int) -> UInt16 { (UInt16(bytes[offset]) << 8) | UInt16(bytes[offset + 1]) }
        guard u16(0) == transaction, u16(2) & 0xF800 == 0x8000,
              u16(2) & 0x0200 == 0, u16(2) & 0x004F == 0,
              u16(4) == 1 else { throw CampusPhysicalDNSError.invalidResponse }
        var offset = 12
        let question = try decodeName(bytes, offset: &offset)
        guard question == host, offset + 4 <= bytes.count, u16(offset) == 1, u16(offset + 2) == 1 else {
            throw CampusPhysicalDNSError.invalidResponse
        }
        offset += 4
        let answerCount = Int(u16(6)), total = answerCount + Int(u16(8)) + Int(u16(10))
        guard total <= 256 else { throw CampusPhysicalDNSError.invalidResponse }
        struct Answer { let name: String; let cname: String?; let address: String? }
        var answers: [Answer] = []
        for record in 0..<total {
            let name = try decodeName(bytes, offset: &offset)
            guard offset + 10 <= bytes.count else { throw CampusPhysicalDNSError.invalidResponse }
            let type = u16(offset), klass = u16(offset + 2), length = Int(u16(offset + 8))
            offset += 10
            let end = offset + length
            guard end <= bytes.count else { throw CampusPhysicalDNSError.invalidResponse }
            if record < answerCount && klass == 1 {
                if type == 1 {
                    guard length == 4 else { throw CampusPhysicalDNSError.invalidResponse }
                    let address = bytes[offset..<end].map(String.init).joined(separator: ".")
                    guard isUsableIPv4(address) else { throw CampusPhysicalDNSError.invalidResponse }
                    answers.append(.init(name: name, cname: nil, address: address))
                } else if type == 5 {
                    var cursor = offset
                    let cname = try decodeName(bytes, offset: &cursor)
                    guard cursor == end, !cname.isEmpty else { throw CampusPhysicalDNSError.invalidResponse }
                    answers.append(.init(name: name, cname: cname, address: nil))
                }
            }
            offset = end
        }
        guard offset == bytes.count else { throw CampusPhysicalDNSError.invalidResponse }
        var current = host, visited: Set<String> = []
        for _ in 0..<16 {
            guard visited.insert(current).inserted else { throw CampusPhysicalDNSError.invalidResponse }
            let matching = answers.filter { $0.name == current }
            let cnames = Set(matching.compactMap(\.cname))
            let addresses = Array(Set(matching.compactMap(\.address))).sorted()
            guard cnames.count <= 1, cnames.isEmpty || addresses.isEmpty else { throw CampusPhysicalDNSError.invalidResponse }
            if !addresses.isEmpty { return addresses }
            guard let next = cnames.first else { throw CampusPhysicalDNSError.invalidResponse }
            current = next
        }
        throw CampusPhysicalDNSError.invalidResponse
    }

    /// Compressed names are bounded by packet length, hops and expanded DNS name length.
    static func decodeName(_ bytes: [UInt8], offset: inout Int) throws -> String {
        var cursor = offset, next: Int?, labels: [String] = [], seen: Set<Int> = [], size = 0
        for _ in 0..<128 {
            guard cursor < bytes.count, seen.insert(cursor).inserted else { throw CampusPhysicalDNSError.invalidResponse }
            let length = Int(bytes[cursor])
            if length & 0xC0 == 0xC0 {
                guard cursor + 1 < bytes.count else { throw CampusPhysicalDNSError.invalidResponse }
                let pointer = ((length & 0x3F) << 8) | Int(bytes[cursor + 1])
                // RFC 1035 compression points to a prior name, never arbitrary later data.
                guard pointer >= 12, pointer < cursor else { throw CampusPhysicalDNSError.invalidResponse }
                if next == nil { next = cursor + 2 }
                cursor = pointer
                continue
            }
            guard length & 0xC0 == 0, length <= 63 else { throw CampusPhysicalDNSError.invalidResponse }
            cursor += 1
            if length == 0 {
                offset = next ?? cursor
                return labels.joined(separator: ".").lowercased()
            }
            guard cursor + length <= bytes.count, size + length + 1 <= 255 else { throw CampusPhysicalDNSError.invalidResponse }
            let label = bytes[cursor..<(cursor + length)]
            guard label.allSatisfy({ (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 45 }) else {
                throw CampusPhysicalDNSError.invalidResponse
            }
            labels.append(String(decoding: label, as: UTF8.self)); size += length + 1; cursor += length
        }
        throw CampusPhysicalDNSError.invalidResponse
    }

    enum ExchangeEvent {
        case packet(server: Int, data: Data)
        case failed(server: Int)
    }

    private enum ExchangeReply {
        case answer([String]), terminalFailure, ignored
    }

    /// Only a response to this exact A/IN question can finish a resolver early. A wrong
    /// transaction, question, opcode or reserved flag remains stray/malformed traffic.
    private static func exchangeReply(_ data: Data, host: String, transaction: UInt16) -> ExchangeReply {
        guard data.count >= 12, data.count <= 65_535 else { return .ignored }
        let bytes = [UInt8](data)
        func u16(_ offset: Int) -> UInt16 { (UInt16(bytes[offset]) << 8) | UInt16(bytes[offset + 1]) }
        let flags = u16(2)
        guard u16(0) == transaction, flags & 0xF800 == 0x8000,
              flags & 0x0040 == 0, u16(4) == 1 else { return .ignored }
        var offset = 12
        guard let question = try? decodeName(bytes, offset: &offset), question == host,
              offset + 4 <= bytes.count, u16(offset) == 1, u16(offset + 2) == 1 else { return .ignored }
        offset += 4
        // A truncated UDP answer supplies no address. Do not silently use TCP, a public
        // resolver or system/TUN DNS. Other associated resolvers may still answer.
        if flags & 0x0200 != 0 || flags & 0x000F != 0 { return .terminalFailure }
        if u16(6) == 0, u16(8) == 0, u16(10) == 0, offset == bytes.count { return .terminalFailure }
        guard let addresses = try? parse(data, host: host, transaction: transaction) else { return .ignored }
        return .answer(addresses)
    }

    /// A synchronous seam for deterministic packet-loss/cancellation tests. Socket creation,
    /// interface/source binding and peer verification remain in resolveA.
    static func exchange(host: String, transaction: UInt16, servers: Int, deadline: TimeInterval,
                         now: () -> TimeInterval, send: (Int, Data) -> Bool,
                         receive: ([Int], TimeInterval) throws -> [ExchangeEvent],
                         cancelled: () -> Bool) throws -> [String] {
        if cancelled() { throw CampusPhysicalDNSError.cancelled }
        guard allowedHosts.contains(host), (1...8).contains(servers), deadline.isFinite else {
            throw CampusPhysicalDNSError.unavailable
        }
        let request = try query(host: host, transaction: transaction)
        var pending = Array(0..<servers), invalid = false, attempt = 0
        let started = now(), sendOffsets: [TimeInterval] = [0, 0.25, 0.75]
        while true {
            if cancelled() { throw CampusPhysicalDNSError.cancelled }
            guard now() < deadline else { break }
            if attempt < sendOffsets.count, now() >= started + sendOffsets[attempt] {
                attempt += 1
                for server in pending {
                    if cancelled() { throw CampusPhysicalDNSError.cancelled }
                    guard now() < deadline else { throw CampusPhysicalDNSError.timeout }
                    if !send(server, request) { pending.removeAll { $0 == server } }
                }
            }
            guard !pending.isEmpty else {
                throw invalid ? CampusPhysicalDNSError.invalidResponse : CampusPhysicalDNSError.unavailable
            }
            let nextSend = attempt < sendOffsets.count ? started + sendOffsets[attempt] : deadline
            let wait = min(0.05, min(nextSend, deadline) - now())
            if wait <= 0 { continue }
            if cancelled() { throw CampusPhysicalDNSError.cancelled }
            let events = try receive(pending, wait)
            if cancelled() { throw CampusPhysicalDNSError.cancelled }
            guard now() < deadline else { break }
            for event in events {
                switch event {
                case .packet(let server, let data) where pending.contains(server):
                    switch exchangeReply(data, host: host, transaction: transaction) {
                    case .answer(let values):
                        if cancelled() { throw CampusPhysicalDNSError.cancelled }
                        guard now() < deadline else { throw CampusPhysicalDNSError.timeout }
                        return values
                    case .terminalFailure:
                        invalid = true
                        pending.removeAll { $0 == server }
                    case .ignored: invalid = true
                    }
                case .failed(let server): pending.removeAll { $0 == server }
                default: break
                }
            }
            guard !pending.isEmpty else {
                throw invalid ? CampusPhysicalDNSError.invalidResponse : CampusPhysicalDNSError.unavailable
            }
        }
        if cancelled() { throw CampusPhysicalDNSError.cancelled }
        throw invalid ? CampusPhysicalDNSError.invalidResponse : CampusPhysicalDNSError.timeout
    }

    static func resolveA(host: String, route: CampusPhysicalRoute, timeout: TimeInterval = 3,
                         cancelled: @Sendable () -> Bool = { false }) throws -> [String] {
        if cancelled() { throw CampusPhysicalDNSError.cancelled }
        guard allowedHosts.contains(host), timeout.isFinite, timeout > 0, timeout <= 30,
              route.interfaceIndex > 0, !route.addresses.isEmpty, !route.dnsServers.isEmpty,
              route.addresses.allSatisfy(isUsableIPv4), route.dnsServers.allSatisfy(isUsableIPv4),
              route.addresses.count <= 16, route.dnsServers.count <= 8 else { throw CampusPhysicalDNSError.unavailable }
        #if os(macOS)
        guard if_nametoindex(route.interfaceName) == route.interfaceIndex else { throw CampusPhysicalDNSError.unavailable }
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        let transaction = UInt16.random(in: .min ... .max)
        var sockets: [Int32] = []
        defer { sockets.forEach { Darwin.close($0) } }
        // Query each current resolver concurrently within one shared deadline, never extending
        // timeout for stray packets or advancing through a guessed public-resolver list.
        for server in route.dnsServers {
            if cancelled() { throw CampusPhysicalDNSError.cancelled }
            guard ProcessInfo.processInfo.systemUptime < deadline else { throw CampusPhysicalDNSError.timeout }
            let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard fd >= 0 else { continue }
            var boundIndex = route.interfaceIndex
            guard setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &boundIndex, socklen_t(MemoryLayout<UInt32>.size)) == 0,
                  fcntl(fd, F_SETFL, O_NONBLOCK) == 0 else { Darwin.close(fd); continue }
            var local = sockaddr_in()
            local.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); local.sin_family = sa_family_t(AF_INET)
            guard inet_pton(AF_INET, route.addresses[0], &local.sin_addr) == 1 else { Darwin.close(fd); continue }
            let bindResult = withUnsafePointer(to: &local) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
            guard bindResult == 0 else { Darwin.close(fd); continue }
            var remote = sockaddr_in()
            remote.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); remote.sin_family = sa_family_t(AF_INET)
            remote.sin_port = UInt16(53).bigEndian
            guard inet_pton(AF_INET, server, &remote.sin_addr) == 1 else { Darwin.close(fd); continue }
            let connected = withUnsafePointer(to: &remote) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
            guard connected == 0 else { Darwin.close(fd); continue }
            sockets.append(fd)
        }
        guard !sockets.isEmpty else { throw CampusPhysicalDNSError.unavailable }
        return try exchange(host: host, transaction: transaction, servers: sockets.count, deadline: deadline,
            now: { ProcessInfo.processInfo.systemUptime }, send: { server, request in
                let sent = request.withUnsafeBytes { Darwin.send(sockets[server], $0.baseAddress, $0.count, 0) }
                return sent == request.count
            }, receive: { active, wait in
                var events = active.map { pollfd(fd: sockets[$0], events: Int16(POLLIN), revents: 0) }
                let milliseconds = Int32(max(1, min(50, (wait * 1000).rounded(.down))))
                let result = Darwin.poll(&events, nfds_t(events.count), milliseconds)
                if result < 0 {
                    if errno == EINTR { return [] }
                    throw CampusPhysicalDNSError.unavailable
                }
                var receivedEvents: [ExchangeEvent] = []
                for (index, event) in events.enumerated() {
                    if event.revents & Int16(POLLIN) != 0 {
                        var bytes = [UInt8](repeating: 0, count: 65_536)
                        let received = Darwin.recv(event.fd, &bytes, bytes.count, 0)
                        // A connected UDP socket verifies peer; the exchange validates the exact
                        // transaction/question before either using an address or ending a resolver.
                        if received >= 0 {
                            receivedEvents.append(.packet(server: active[index], data: Data(bytes.prefix(received))))
                        } else if errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR {
                            receivedEvents.append(.failed(server: active[index]))
                        }
                    }
                    if event.revents & Int16(POLLERR | POLLHUP | POLLNVAL) != 0 {
                        receivedEvents.append(.failed(server: active[index]))
                    }
                }
                return receivedEvents
            }, cancelled: cancelled)
        #else
        throw CampusPhysicalDNSError.unavailable
        #endif
    }
}
