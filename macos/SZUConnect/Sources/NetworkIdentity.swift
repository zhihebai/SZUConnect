import Foundation
import Network
import CryptoKit
#if os(macOS)
import SystemConfiguration
import SystemConfiguration.SCDynamicStoreCopyDHCPInfo
import Darwin
#endif

/// Ephemeral evidence of path continuity. Never serialize or expose this fingerprint.
struct NetworkIdentity: Equatable, Sendable {
    private let fingerprint: String
    let permitsBriefInterruption: Bool
    init(fingerprint: String, permitsBriefInterruption: Bool = false) {
        self.fingerprint = fingerprint
        self.permitsBriefInterruption = permitsBriefInterruption
    }

    /// Canonicalization is pure so order-only dynamic-store updates can be tested offline.
    static func make(components: [String: [String]]) -> NetworkIdentity? {
        let canonical = components.mapValues { Array(Set($0.filter { !$0.isEmpty })).sorted() }
        guard let interfaces = canonical["interfaces"], !interfaces.isEmpty,
              let services = canonical["services"], !services.isEmpty,
              let routers = canonical["routers"], !routers.isEmpty,
              let primaryService = canonical["primaryService"], !primaryService.isEmpty,
              let primaryInterface = canonical["primaryInterface"], !primaryInterface.isEmpty else { return nil }
        guard let data = try? JSONSerialization.data(withJSONObject: canonical, options: [.sortedKeys]) else { return nil }
        return NetworkIdentity(fingerprint: SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined(),
            permitsBriefInterruption: !(canonical["attachment"] ?? []).isEmpty)
    }
}

struct NetworkContinuitySnapshot: Sendable {
    let identity: NetworkIdentity?
    let linkActive: Bool
    /// Routing evidence shows a VPN / TUN route for at least part of the network.
    var tunnel = false
    /// Selected system physical attachment; connected interfaces are listed independently.
    var physicalLabels: [String] = []
    /// The system's selected physical default, not proof of a tunnel's actual outbound socket.
    var systemPhysicalLabel: String? = nil
    var connectedPhysicalLabels: [String] = []
    /// A multi-link tunnel context may be explicitly trusted only for this process and topology.
    var sessionOnlyTrust = false
    /// The credential channel must use this exact live physical lease when present.
    var authenticationRoute: CampusPhysicalRoute? = nil
    var requiresPhysicalBinding = false
    var hasUsablePhysicalLink = false
    var trustUnavailableReason: String?
    /// RAM-only features of the selected physical egress; other links cannot supply authentication authority.
    var descriptors: [NetworkDescriptor] = []
    /// Local display metadata only, captured from the selected interface and never hashed or exported.
    var label: NetworkObservedLabel?
    /// Name metadata for live Wi-Fi attachments, even when Ethernet is the selected egress.
    /// These pairs never enter the authorization descriptors or ephemeral identity.
    struct WiFiLabelAttachment: Sendable {
        let descriptor: NetworkDescriptor
        let label: NetworkObservedLabel
    }
    var wifiLabelAttachments: [WiFiLabelAttachment] = []

    /// One system network service as published in the dynamic store; only what selection needs.
    struct ServiceState: Equatable, Sendable {
        let interface: String
        let hasRouter: Bool
        var linkActive = true
        var router: String? = nil
    }

    struct PhysicalDefault: Equatable, Hashable, Sendable {
        let interface: String
        let gateway: String
    }

    struct PhysicalSelection: Equatable, Sendable {
        let service: String
        let interface: String
        let viaTunnel: Bool
        var requiresCompositeTrust = false
    }

    /// Pure and offline-testable. Without a tunnel, the global primary
    /// service must itself be a live physical Wi-Fi/Ethernet service with a router. A tunnel
    /// permits fallback when one physical candidate is active. With several candidates, a
    /// matching system physical default permits explicit session trust of the whole topology.
    /// Neither service preference nor this default proves the tunnel's actual outbound socket.
    static func selectPhysicalService(primaryService: String?, primaryInterface: String?,
                                      serviceOrder: [String], services: [String: ServiceState],
                                      physicalInterfaces: Set<String>, routingTunnel: Bool = false,
                                      physicalDefaults: [PhysicalDefault] = []) -> PhysicalSelection? {
        guard let primaryService, !primaryService.isEmpty,
              let primaryInterface, !primaryInterface.isEmpty else { return nil }
        if physicalInterfaces.contains(primaryInterface) && !routingTunnel {
            guard let state = services[primaryService], state.interface == primaryInterface,
                  state.hasRouter, state.linkActive else { return nil }
            return PhysicalSelection(service: primaryService, interface: primaryInterface, viaTunnel: false)
        }
        func eligible(_ id: String) -> Bool {
            guard (routingTunnel || id != primaryService), let state = services[id] else { return false }
            return state.hasRouter && state.linkActive && physicalInterfaces.contains(state.interface)
        }
        // Service order is a preference, not proof of a VPN's physical egress.
        // Several candidates need independent physical-default evidence and composite trust.
        let candidates = services.keys.sorted().filter(eligible)
        if routingTunnel, candidates.count > 1, physicalInterfaces.contains(primaryInterface),
           let state = services[primaryService], eligible(primaryService), state.interface == primaryInterface,
           let router = state.router, !router.isEmpty {
            let defaults = Set(physicalDefaults)
            guard defaults.count == 1, let selected = defaults.first,
                  selected.interface == primaryInterface, selected.gateway == router else { return nil }
            return PhysicalSelection(service: primaryService, interface: primaryInterface, viaTunnel: true,
                                     requiresCompositeTrust: true)
        }
        guard candidates.count == 1 else { return nil }
        guard let chosen = candidates.first, let state = services[chosen] else { return nil }
        return PhysicalSelection(service: chosen, interface: state.interface, viaTunnel: true)
    }

    static func memoryDescriptors(for selection: PhysicalSelection?,
                                  byService descriptors: [String: NetworkDescriptor]) -> [NetworkDescriptor] {
        guard let selection, !selection.requiresCompositeTrust,
              let descriptor = descriptors[selection.service] else { return [] }
        return [descriptor]
    }

    /// RAM-only associated records. Sorting fields independently across services would lose
    /// which router/address belongs to which attachment and could preserve trust after a swap.
    static func compositeAttachmentComponents(services: [String: ServiceState], details: [String: [String: Any]],
                                              physicalInterfaces: Set<String>) -> [String]? {
        var records: [String] = []
        for (id, state) in services where physicalInterfaces.contains(state.interface) && state.hasRouter && state.linkActive {
            guard let item = details[id], item["InterfaceName"] as? String == state.interface,
                  let router = item["Router"] as? String, !router.isEmpty, router == state.router,
                  let addresses = item["Addresses"] as? [String], !addresses.isEmpty,
                  let masks = item["SubnetMasks"] as? [String], !masks.isEmpty else { return nil }
            guard masks.count == 1 || masks.count == addresses.count else { return nil }
            let addressMasks = addresses.enumerated().map { index, address in
                address + "/" + masks[masks.count == 1 ? 0 : index]
            }
            let fields: [String: [String]] = [
                "service": [id], "interface": [state.interface], "router": [router],
                "addresses": addresses, "masks": masks, "addressMasks": addressMasks,
                "signature": [item["NetworkSignature"] as? String ?? ""],
                "hardware": [item["ARPResolvedHardwareAddress"] as? String ?? ""]
            ]
            let canonical = fields.mapValues { Array(Set($0.filter { !$0.isEmpty })).sorted() }
            guard let data = try? JSONSerialization.data(withJSONObject: canonical, options: [.sortedKeys]),
                  let record = String(data: data, encoding: .utf8) else { return nil }
            records.append(record)
        }
        return records.isEmpty ? nil : Array(Set(records)).sorted()
    }

    /// Pure and offline-testable: one memory descriptor from a published IPv4 service dictionary plus optional
    /// DHCP / DNS records. The client address is used only to derive the subnet and is never kept.
    static func makeDescriptor(kind: String, ipv4 item: [String: Any], dhcpServer: String? = nil,
                               dnsServers: [String] = [], searchDomain: String? = nil) -> NetworkDescriptor {
        var raw: [String: String] = [:]
        let router = ((item["Router"] as? String) ?? "").trimmingCharacters(in: .whitespaces)
        if !router.isEmpty { raw["gwip"] = router }
        var hardware = (item["ARPResolvedHardwareAddress"] as? String) ?? ""
        if hardware.isEmpty, let signature = item["NetworkSignature"] as? String,
           let range = signature.range(of: "IPv4.RouterHardwareAddress=") {
            hardware = String(signature[range.upperBound...].prefix { $0 != ";" && $0 != "," })
        }
        if let normalized = normalizedHardwareAddress(hardware) { raw["gw"] = normalized }
        if let addresses = item["Addresses"] as? [String], let masks = item["SubnetMasks"] as? [String],
           let address = addresses.first, let mask = masks.first, let network = networkText(address: address, mask: mask) {
            raw["net"] = network
        }
        if let dhcpServer, !dhcpServer.isEmpty { raw["dhcp"] = dhcpServer }
        if !dnsServers.isEmpty { raw["dns"] = dnsServers.sorted().joined(separator: ",") }
        if let searchDomain, !searchDomain.isEmpty { raw["sfx"] = searchDomain.lowercased() }
        return NetworkDescriptor(kind: kind, raw: raw)
    }

    static func normalizedHardwareAddress(_ text: String) -> String? {
        let parts = text.lowercased().split(separator: ":").map(String.init)
        guard parts.count == 6 else { return nil }
        var out: [String] = []
        for part in parts {
            guard (1...2).contains(part.count), part.allSatisfy({ $0.isHexDigit }) else { return nil }
            out.append(part.count == 1 ? "0" + part : part)
        }
        return out.allSatisfy({ $0 == "00" }) ? nil : out.joined(separator: ":")
    }

    static func networkText(address: String, mask: String) -> String? {
        func octets(_ text: String) -> [UInt32]? {
            let parts = text.split(separator: ".").compactMap { UInt32($0) }
            return parts.count == 4 && text.split(separator: ".").count == 4 && parts.allSatisfy({ $0 < 256 }) ? parts : nil
        }
        guard let a = octets(address), let m = octets(mask), !(a[0] == 169 && a[1] == 254) else { return nil }
        let ip = (a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]
        let bits = (m[0] << 24) | (m[1] << 16) | (m[2] << 8) | m[3]
        let prefix = bits.nonzeroBitCount
        let expected: UInt32 = prefix == 32 ? 0xFFFF_FFFF : (~UInt32(0)) << UInt32(32 - prefix)
        guard prefix > 0, bits == expected else { return nil }
        let network = ip & bits
        return "\(network >> 24).\((network >> 16) & 255).\((network >> 8) & 255).\(network & 255)/\(prefix)"
    }

    /// Read-only OS metadata. Wi-Fi names are read only with existing permission; never requests permission or changes networking.
    static func capture(path: NWPath) -> Self {
        #if os(macOS)
        guard let store = SCDynamicStoreCreate(nil, "local.SZUConnect.continuity" as CFString, nil, nil)
        else { return .init(identity: nil, linkActive: false) }
        var kinds: [String: String] = [:]

        for interface in path.availableInterfaces where interface.type == .wifi || interface.type == .wiredEthernet {
            kinds[interface.name] = interface.type == .wifi ? "Wi-Fi" : "有线"
        }
        let global = SCDynamicStoreCopyValue(store, "State:/Network/Global/IPv4" as CFString) as? [String: Any] ?? [:]
        let values = SCDynamicStoreCopyMultiple(store, nil,
            ["State:/Network/Service/.*/IPv4"] as CFArray) as? [String: Any] ?? [:]
        var states: [String: ServiceState] = [:]
        var details: [String: [String: Any]] = [:]
        var links: [String: Bool] = [:]
        for (key, value) in values {
            // Keys look like State:/Network/Service/<service id>/IPv4.
            let parts = key.split(separator: "/")
            guard parts.count == 5, let item = value as? [String: Any],
                  let name = item["InterfaceName"] as? String, !name.isEmpty else { continue }
            let id = String(parts[3])
            if links[name] == nil {
                let link = SCDynamicStoreCopyValue(store, "State:/Network/Interface/\(name)/Link" as CFString) as? [String: Any]
                links[name] = link?["Active"] as? Bool == true
            }
            states[id] = ServiceState(interface: name,
                hasRouter: !((item["Router"] as? String) ?? "").isEmpty, linkActive: links[name] == true,
                router: item["Router"] as? String)
            details[id] = item
        }
        // A tunnel can hide the physical interface from NWPath; SystemConfiguration still knows its type.
        let hidden = Set(states.values.map { $0.interface }).subtracting(kinds.keys)
        if !hidden.isEmpty {
            for (name, kind) in physicalKinds(named: hidden) { kinds[name] = kind }
        }
        let physical = Set(kinds.keys)
        let primaryService = global["PrimaryService"] as? String
        let primaryInterface = global["PrimaryInterface"] as? String
        let routes = TunnelRouteSnapshot.capture(physicalInterfaces: physical)
        let routedThroughOther = path.usesInterfaceType(.other)
        let routedThroughTunnel = routes?.hasTunnelRoutes == true
        let activePhysical = states.values.filter { physical.contains($0.interface) && $0.hasRouter && $0.linkActive }
        let physicalLabels = Array(Set(activePhysical.compactMap { state in kinds[state.interface].map { $0 + " " + state.interface } })).sorted()
        // Memory descriptors of every live physical service (also under a tunnel; never the client address itself).
        var descriptorsByService: [String: NetworkDescriptor] = [:]
        for (id, state) in states.sorted(by: { $0.key < $1.key }) where physical.contains(state.interface) && state.hasRouter && state.linkActive {
            guard let item = details[id], let kind = kinds[state.interface] else { continue }
            let dhcp = SCDynamicStoreCopyDHCPInfo(store, id as CFString)
                .flatMap { DHCPInfoGetOptionData($0, 54) as Data? }
                .flatMap { $0.count == 4 ? $0.map { String($0) }.joined(separator: ".") : nil }
            let dns = SCDynamicStoreCopyValue(store, "State:/Network/Service/\(id)/DNS" as CFString) as? [String: Any]
            descriptorsByService[id] = makeDescriptor(kind: kind == "Wi-Fi" ? "wifi" : "ethernet", ipv4: item, dhcpServer: dhcp,
                                              dnsServers: dns?["ServerAddresses"] as? [String] ?? [],
                                              searchDomain: (dns?["DomainName"] as? String) ?? (dns?["SearchDomains"] as? [String])?.first)
        }
        let wifiLabelAttachments: [WiFiLabelAttachment] = descriptorsByService.keys.sorted().compactMap { id in
            guard let descriptor = descriptorsByService[id], descriptor.kind == "wifi", descriptor.isReliable,
                  let state = states[id], state.linkActive else { return nil }
            let label = NetworkObservedLabel.capture(interface: state.interface, kind: .wifi, serviceID: id)
            guard label.ssid != nil else { return nil }
            // Do not attach a newly read SSID to IPv4 metadata that changed during collection.
            guard let original = details[id],
                  let current = SCDynamicStoreCopyValue(store, "State:/Network/Service/\(id)/IPv4" as CFString) as? [String: Any],
                  NSDictionary(dictionary: original).isEqual(to: current) else { return nil }
            return WiFiLabelAttachment(descriptor: descriptor, label: label)
        }
        guard let selection = selectPhysicalService(primaryService: primaryService, primaryInterface: primaryInterface,
                                                    serviceOrder: [],
                                                    services: states, physicalInterfaces: physical, routingTunnel: routedThroughTunnel,
                                                    physicalDefaults: routes?.physicalDefaults ?? []),
              let item = details[selection.service], let kind = kinds[selection.interface] else {
            let virtualPrimary = primaryInterface.map { !physical.contains($0) } ?? false
            return .init(identity: nil, linkActive: false, tunnel: routedThroughOther || virtualPrimary || routedThroughTunnel,
                         connectedPhysicalLabels: physicalLabels, hasUsablePhysicalLink: !activePhysical.isEmpty,
                         trustUnavailableReason: routes == nil
                            ? "暂时读不到完整的网络路由信息。可以继续检测，暂时不能信任此网络。"
                            : activePhysical.count > 1
                            ? "同时连接多个网络，系统物理出口与路由信息尚未一致。可以继续检测，暂时不能信任此网络。"
                            : "暂时无法识别当前网络，不能信任此网络。",
                         descriptors: [], wifiLabelAttachments: wifiLabelAttachments)
        }
        let tunnel = selection.viaTunnel || routedThroughOther || routedThroughTunnel
        let active = links[selection.interface] == true
        // Some macOS versions add a router/attachment signature. It strengthens evidence
        // when present; public interface, service and route fields remain the baseline.
        var signatures: [String] = []
        if let signature = item["NetworkSignature"] as? String { signatures.append(signature) }
        if let routerAddress = item["ARPResolvedHardwareAddress"] as? String { signatures.append(routerAddress) }
        var components: [String: [String]] = [
            "interfaces": [stableInterfaceComponent(selection.interface, kind: kind)],
            "services": [selection.service],
            "routers": [item["Router"] as? String ?? ""],
            "addresses": item["Addresses"] as? [String] ?? [],
            "primaryService": [selection.service], "primaryInterface": [selection.interface],
            "pathGateways": path.gateways.map { String(describing: $0) },
            "masks": item["SubnetMasks"] as? [String] ?? [], "attachment": signatures,
            // Switching a VPN/TUN proxy on or off changes the route and must be re-confirmed.
            "route": (tunnel ? ["tunnel", primaryService ?? "", primaryInterface ?? ""] : ["direct"])
                + (routes?.identityComponents ?? [])
        ]
        var unavailableReason = routes == nil
            ? "暂时读不到完整的网络信息。可以继续检测，暂时不能信任此网络。" : nil
        if selection.requiresCompositeTrust {
            if let attachments = compositeAttachmentComponents(services: states, details: details,
                                                                physicalInterfaces: physical) {
                components["physicalAttachments"] = attachments
                // Composite trust never carries across an interruption; every attachment must remain live.
                components["attachment"] = []
            } else {
                unavailableReason = "当前连接信息不完整。可以继续检测，暂时不能信任此网络。"
            }
            // Recheck the associated services and selected primary after collecting all metadata.
            // A mixed snapshot during Wi-Fi/cable switching must never acquire session authority.
            let latestGlobal = SCDynamicStoreCopyValue(store, "State:/Network/Global/IPv4" as CFString) as? [String: Any]
            let latestValues = SCDynamicStoreCopyMultiple(store, nil,
                ["State:/Network/Service/.*/IPv4"] as CFArray) as? [String: Any]
            var latestLive: [String: [String: Any]] = [:]
            for (key, value) in latestValues ?? [:] {
                let parts = key.split(separator: "/")
                guard parts.count == 5, let current = value as? [String: Any],
                      let name = current["InterfaceName"] as? String, physical.contains(name),
                      !((current["Router"] as? String) ?? "").isEmpty else { continue }
                let link = SCDynamicStoreCopyValue(store, "State:/Network/Interface/\(name)/Link" as CFString) as? [String: Any]
                if link?["Active"] as? Bool == true { latestLive[String(parts[3])] = current }
            }
            let originalLive = states.filter { physical.contains($0.value.interface) && $0.value.hasRouter && $0.value.linkActive }
            let stable = latestGlobal?["PrimaryService"] as? String == primaryService
                && latestGlobal?["PrimaryInterface"] as? String == primaryInterface
                && Set(latestLive.keys) == Set(originalLive.keys)
                && originalLive.keys.allSatisfy { id in
                    guard let original = details[id], let current = latestLive[id] else { return false }
                    return NSDictionary(dictionary: original).isEqual(to: current)
                }
                && TunnelRouteSnapshot.capture(physicalInterfaces: physical) == routes
            if !stable { unavailableReason = "网络正在切换，系统物理出口或连接信息已变化。请稍后再信任。" }
        }
        let identity = unavailableReason == nil ? NetworkIdentity.make(components: components) : nil
        let selectedLabel = kind + " " + selection.interface
        return .init(identity: identity, linkActive: active, tunnel: tunnel,
                     physicalLabels: [selectedLabel], systemPhysicalLabel: selectedLabel,
                     connectedPhysicalLabels: physicalLabels, sessionOnlyTrust: selection.requiresCompositeTrust,
                     hasUsablePhysicalLink: active, trustUnavailableReason: unavailableReason,
                     descriptors: identity == nil ? [] : memoryDescriptors(for: selection, byService: descriptorsByService),
                     label: identity == nil || selection.requiresCompositeTrust ? nil : NetworkObservedLabel.capture(interface: selection.interface,
                        kind: kind == "Wi-Fi" ? .wifi : .ethernet, serviceID: selection.service),
                     wifiLabelAttachments: wifiLabelAttachments)
        #else
        // iOS does not expose the same system configuration APIs without additional access.
        // Retain conservative per-notification permission rather than inventing Wi-Fi identity.
        return .init(identity: path.status == .satisfied ? NetworkIdentity(fingerprint: UUID().uuidString) : nil,
                     linkActive: path.status == .satisfied)
        #endif
    }

    /// Canonical across NWPath visibility and the SystemConfiguration fallback.
    static func stableInterfaceComponent(_ name: String, kind: String) -> String {
        let normalized: String
        switch kind {
        case "Wi-Fi", "wifi": normalized = "wifi"
        case "有线", "wiredEthernet", "Ethernet": normalized = "ethernet"
        default: normalized = kind
        }
        return name + ":" + normalized
    }

    #if os(macOS)
    /// Maps BSD names to "Wi-Fi" / "有线" using SystemConfiguration interface types.
    private static func physicalKinds(named names: Set<String>) -> [String: String] {
        guard let all = SCNetworkInterfaceCopyAll() as? [SCNetworkInterface] else { return [:] }
        var result: [String: String] = [:]
        for interface in all {
            guard let name = SCNetworkInterfaceGetBSDName(interface) as String?, names.contains(name),
                  let type = SCNetworkInterfaceGetInterfaceType(interface) as String? else { continue }
            if type == (kSCNetworkInterfaceTypeIEEE80211 as String) {
                result[name] = "Wi-Fi"
            } else if type == (kSCNetworkInterfaceTypeEthernet as String) {
                result[name] = "有线"
            }
        }
        return result
    }
    #endif
}

/// Everything AppModel needs from one NWPath update, captured off the main actor.
struct PathObservation: Sendable {
    let usable: Bool
    let interfaces: String
    let constrained: Bool
    let continuity: NetworkContinuitySnapshot
    var physicalCandidates: [CampusPhysicalRoute] = []

    static func permitsProbe(pathSatisfied: Bool, physicalPath: Bool, tunnelUsable: Bool) -> Bool {
        pathSatisfied && (physicalPath || tunnelUsable)
    }

    static func capture(_ path: NWPath) -> PathObservation {
        var continuity = NetworkContinuitySnapshot.capture(path: path)
        #if os(macOS)
        continuity.requiresPhysicalBinding = true
        let candidates = CampusPhysicalNetwork.candidates()
        #else
        let candidates: [CampusPhysicalRoute] = []
        #endif
        let physical = path.usesInterfaceType(.wifi) || path.usesInterfaceType(.wiredEthernet)
        #if os(macOS)
        // A full-tunnel VPN or TUN proxy can hide Wi-Fi/Ethernet from NWPath. Accept that path
        // when the system snapshot proves a physical service with a router and live link.
        let tunnelUsable = path.status == .satisfied && !physical && continuity.tunnel
            && continuity.hasUsablePhysicalLink && !continuity.connectedPhysicalLabels.isEmpty
        #else
        let tunnelUsable = false
        #endif
        var labels = path.availableInterfaces.filter { interface in
            (interface.type == .wifi || interface.type == .wiredEthernet) && path.usesInterfaceType(interface.type)
        }.map { ($0.type == .wifi ? "Wi-Fi " : "有线 ") + $0.name }
        if !continuity.physicalLabels.isEmpty { labels = continuity.physicalLabels }
        else if !continuity.connectedPhysicalLabels.isEmpty { labels = continuity.connectedPhysicalLabels }
        return PathObservation(usable: permitsProbe(pathSatisfied: path.status == .satisfied,
                                                    physicalPath: physical, tunnelUsable: tunnelUsable),
                               interfaces: Array(Set(labels)).sorted().joined(separator: " · "),
                               constrained: path.isConstrained || path.isExpensive,
                               continuity: continuity, physicalCandidates: candidates)
    }
}

#if os(macOS)
/// A bounded, read-only routing-table snapshot. A present utun interface alone proves nothing:
/// macOS keeps idle scoped IPv6 defaults on several utun interfaces. Only usable, non-local
/// routes on a live tunnel count; scoped entries additionally need a specific public destination. This describes routing, never a successful VPN handshake.
struct TunnelRouteSnapshot: Equatable, Sendable {
    struct Interface: Equatable, Sendable {
        let name: String
        let up: Bool
        let running: Bool
    }
    struct Route: Equatable, Sendable {
        let family: Int32
        let destination: [UInt8]
        let prefixLength: Int
        let gateway: [UInt8]
        let interfaceIndex: UInt16
        let flags: Int32
    }

    let identityComponents: [String]
    var physicalDefaults: [NetworkContinuitySnapshot.PhysicalDefault] = []
    /// Defaults usable by an interface-bound socket, including secondary interface scopes.
    /// These never replace the unique unscoped default used to identify the system primary.
    var boundPhysicalDefaults: [NetworkContinuitySnapshot.PhysicalDefault] = []
    var hasTunnelRoutes: Bool { !identityComponents.isEmpty }
    static let maximumBytes = 4 * 1_024 * 1_024
    static let maximumRoutes = 16_384

    static func classify(routes: [Route], interfaces: [UInt16: Interface], physicalInterfaces: Set<String> = []) -> Self {
        let excluded = RTF_REJECT | RTF_BLACKHOLE | RTF_WASCLONED
            | RTF_LLINFO | RTF_LOCAL | RTF_BROADCAST | RTF_MULTICAST | RTF_CONDEMNED | RTF_DEAD
        let components = routes.compactMap { route -> String? in
            guard route.flags & RTF_UP != 0,
                  route.flags & excluded == 0, let interface = interfaces[route.interfaceIndex],
                  interface.up, interface.running, isTunnelName(interface.name),
                  isNonLocalDestination(route) else { return nil }
            let scoped = route.flags & RTF_IFSCOPE != 0
            if scoped {
                // Scoped defaults are installed for idle system utuns. A configured scoped
                // public split route is useful evidence, but does not prove an unbound request uses it.
                guard route.prefixLength > 0, hasPublicDestinationEvidence(route) else { return nil }
            } else if route.flags & RTF_GATEWAY == 0 {
                // Packet tunnels may install interface routes (link#) without RTF_GATEWAY.
                // Defaults / split defaults are affirmative evidence; a private connected
                // point-to-point subnet alone is indistinguishable from an idle attachment.
                guard route.prefixLength <= 1 || hasPublicDestinationEvidence(route) else { return nil }
            }
            let destination = route.destination.map { String(format: "%02x", $0) }.joined()
            let gateway = route.gateway.map { String(format: "%02x", $0) }.joined()
            return "\(interface.name):\(route.family):\(destination)/\(route.prefixLength):\(gateway):\(scoped ? "scoped" : "unscoped")"
        }
        return .init(identityComponents: Array(Set(components)).sorted(),
                     physicalDefaults: physicalIPv4Defaults(routes: routes, interfaces: interfaces,
                                                           physicalInterfaces: physicalInterfaces),
                     boundPhysicalDefaults: physicalIPv4Defaults(routes: routes, interfaces: interfaces,
                                                           physicalInterfaces: physicalInterfaces, includeScoped: true))
    }

    /// Only an unscoped system IPv4 default on a known live physical interface is evidence.
    /// This says nothing about a TUN implementation's separate outbound-interface policy.
    static func physicalIPv4Defaults(routes: [Route], interfaces: [UInt16: Interface],
                                     physicalInterfaces: Set<String>, includeScoped: Bool = false) -> [NetworkContinuitySnapshot.PhysicalDefault] {
        let excluded = RTF_REJECT | RTF_BLACKHOLE | RTF_WASCLONED | RTF_HOST
            | RTF_LLINFO | RTF_LOCAL | RTF_BROADCAST | RTF_MULTICAST | RTF_CONDEMNED | RTF_DEAD
        let defaults = routes.compactMap { route -> NetworkContinuitySnapshot.PhysicalDefault? in
            guard route.family == AF_INET, route.destination == [0, 0, 0, 0], route.prefixLength == 0,
                  route.flags & (RTF_UP | RTF_GATEWAY) == (RTF_UP | RTF_GATEWAY), route.flags & excluded == 0,
                  includeScoped || route.flags & RTF_IFSCOPE == 0,
                  let interface = interfaces[route.interfaceIndex], interface.up, interface.running,
                  physicalInterfaces.contains(interface.name),
                  route.gateway.count == MemoryLayout<sockaddr_in>.size,
                  Int(route.gateway[0]) == route.gateway.count, route.gateway[1] == UInt8(AF_INET) else { return nil }
            let address = Array(route.gateway[4..<8])
            guard address[0] != 0, address[0] != 127, address[0] < 224 else { return nil }
            return .init(interface: interface.name, gateway: address.map(String.init).joined(separator: "."))
        }
        return Array(Set(defaults)).sorted { ($0.interface, $0.gateway) < ($1.interface, $1.gateway) }
    }

    private static func isTunnelName(_ name: String) -> Bool {
        for prefix in ["utun", "tun", "tap", "ppp", "ipsec"] where name.hasPrefix(prefix) {
            let suffix = name.dropFirst(prefix.count)
            if !suffix.isEmpty && suffix.allSatisfy({ $0.isASCII && $0.isNumber }) { return true }
        }
        return false
    }

    private static func isNonLocalDestination(_ route: Route) -> Bool {
        let bytes = route.destination
        if route.family == AF_INET {
            guard bytes.count == 4, (0...32).contains(route.prefixLength) else { return false }
            if route.prefixLength == 0 { return bytes.allSatisfy { $0 == 0 } }
            // Do not mistake loopback, link-local or multicast housekeeping for traffic capture.
            return !(route.prefixLength >= 8 && (bytes[0] == 0 || bytes[0] == 127))
                && !(route.prefixLength >= 16 && bytes[0] == 169 && bytes[1] == 254)
                && !(route.prefixLength >= 4 && bytes[0] >= 224)
        }
        if route.family == AF_INET6 {
            guard bytes.count == 16, (0...128).contains(route.prefixLength) else { return false }
            if route.prefixLength == 0 { return bytes.allSatisfy { $0 == 0 } }
            return !(route.prefixLength >= 10 && bytes[0] == 0xfe && bytes[1] & 0xc0 == 0x80)
                && !(route.prefixLength >= 8 && bytes[0] == 0xff)
                && !(route.prefixLength == 128 && bytes.dropLast().allSatisfy { $0 == 0 } && bytes.last! <= 1)
        }
        return false
    }

    /// Conservative positive evidence for a public target, not a reachability/assignment test.
    /// Broad split prefixes qualify when they are not wholly inside a special-use block.
    private static func hasPublicDestinationEvidence(_ route: Route) -> Bool {
        func contained(_ network: [UInt8], _ prefix: Int) -> Bool {
            guard route.prefixLength >= prefix, network.count == route.destination.count else { return false }
            let whole = prefix / 8
            if !zip(route.destination.prefix(whole), network.prefix(whole)).allSatisfy({ $0 == $1 }) { return false }
            let remaining = prefix % 8
            return remaining == 0 || (route.destination[whole] & (0xff << (8 - remaining)))
                == (network[whole] & (0xff << (8 - remaining)))
        }
        if route.family == AF_INET {
            let special: [([UInt8], Int)] = [
                ([0, 0, 0, 0], 8), ([10, 0, 0, 0], 8), ([100, 64, 0, 0], 10),
                ([127, 0, 0, 0], 8), ([169, 254, 0, 0], 16), ([172, 16, 0, 0], 12),
                ([192, 0, 0, 0], 24), ([192, 0, 2, 0], 24), ([192, 168, 0, 0], 16),
                ([198, 18, 0, 0], 15), ([198, 51, 100, 0], 24), ([203, 0, 113, 0], 24),
                ([224, 0, 0, 0], 4), ([240, 0, 0, 0], 4)
            ]
            return !special.contains { contained($0.0, $0.1) }
        }
        if route.family == AF_INET6 {
            // Current global-unicast space is 2000::/3. Include a broader prefix that
            // intersects it, but never scoped ULA/link-local point-to-point attachments.
            let significantBits = min(route.prefixLength, 3)
            let mask: UInt8 = significantBits == 0 ? 0 : 0xff << (8 - significantBits)
            guard route.destination[0] & mask == 0x20 & mask else { return false }
            let documentation: [UInt8] = [0x20, 0x01, 0x0d, 0xb8] + Array(repeating: 0, count: 12)
            return !contained(documentation, 32)
        }
        return false
    }

    /// Decodes NET_RT_DUMP2 using SDK struct layouts and Darwin's 4-byte sockaddr alignment.
    /// Any truncated/invalid message rejects the snapshot instead of accepting partial evidence.
    static func decode(_ data: Data) -> [Route]? {
        guard data.count <= maximumBytes else { return nil }
        let bytes = [UInt8](data)
        let headerSize = MemoryLayout<rt_msghdr2>.size
        var routes: [Route] = []
        var offset = 0
        var messages = 0
        while offset < bytes.count {
            guard bytes.count - offset >= headerSize, messages < maximumRoutes else { return nil }
            let header = bytes.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: rt_msghdr2.self) }
            let length = Int(header.rtm_msglen)
            guard length >= headerSize, length <= bytes.count - offset,
                  header.rtm_version == RTM_VERSION,
                  header.rtm_type == RTM_GET2 || header.rtm_type == RTM_GET else { return nil }
            let end = offset + length
            var cursor = offset + headerSize
            var addresses: [Int: [UInt8]] = [:]
            // Unknown address bits cannot be decoded safely by this version.
            guard header.rtm_addrs & ~((1 << RTAX_MAX) - 1) == 0 else { return nil }
            for index in 0..<Int(RTAX_MAX) where header.rtm_addrs & (1 << index) != 0 {
                guard cursor < end else { return nil }
                let size = Int(bytes[cursor])
                let aligned = size == 0 ? 4 : (size + 3) & ~3
                guard (size == 0 || size >= 2), aligned <= end - cursor else { return nil }
                addresses[index] = Array(bytes[cursor..<(cursor + size)])
                cursor += aligned
            }
            if let dst = addresses[Int(RTAX_DST)], dst.count >= 2 {
                let family = Int32(dst[1])
                if family == AF_INET || family == AF_INET6 {
                    let addressOffset = family == AF_INET ? 4 : 8
                    let width = family == AF_INET ? 4 : 16
                    guard dst.count >= addressOffset + width else { return nil }
                    let destination = Array(dst[addressOffset..<(addressOffset + width)])
                    let prefix: Int
                    if header.rtm_flags & RTF_HOST != 0 {
                        prefix = width * 8
                    } else if let mask = addresses[Int(RTAX_NETMASK)] {
                        var expanded = [UInt8](repeating: 0, count: width)
                        if mask.count > addressOffset {
                            for i in 0..<min(width, mask.count - addressOffset) { expanded[i] = mask[addressOffset + i] }
                        }
                        guard let value = prefixLength(expanded) else { return nil }
                        prefix = value
                    } else {
                        // Missing mask is legitimate for a default route, never a guessed host route.
                        guard destination.allSatisfy({ $0 == 0 }) else { return nil }
                        prefix = 0
                    }
                    let gateway = addresses[Int(RTAX_GATEWAY)] ?? []
                    routes.append(Route(family: family, destination: destination, prefixLength: prefix,
                                        gateway: gateway, interfaceIndex: header.rtm_index, flags: header.rtm_flags))
                }
            }
            offset = end
            messages += 1
        }
        return routes
    }

    private static func prefixLength(_ mask: [UInt8]) -> Int? {
        var ones = 0
        var foundZero = false
        for byte in mask {
            for bit in (0..<8).reversed() {
                if byte & (1 << bit) == 0 { foundZero = true }
                else if foundZero { return nil }
                else { ones += 1 }
            }
        }
        return ones
    }

    static func capture(physicalInterfaces: Set<String> = []) -> Self? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0 else { return nil }
        defer { freeifaddrs(list) }
        var interfaces: [UInt16: Interface] = [:]
        var current = list
        while let node = current {
            let item = node.pointee
            if let namePointer = item.ifa_name {
                let index = if_nametoindex(namePointer)
                if index > 0 && index <= UInt16.max {
                    interfaces[UInt16(index)] = Interface(name: String(cString: namePointer),
                        up: item.ifa_flags & UInt32(IFF_UP) != 0,
                        running: item.ifa_flags & UInt32(IFF_RUNNING) != 0)
                }
            }
            current = item.ifa_next
        }
        // Allow one retry if routes changed between sizing and reading. Never spawn a shell,
        // open a routing socket, resolve a host, send a packet or modify system configuration.
        for _ in 0..<2 {
            var mib: [Int32] = [CTL_NET, PF_ROUTE, 0, AF_UNSPEC, NET_RT_DUMP2, 0]
            var count = 0
            guard sysctl(&mib, u_int(mib.count), nil, &count, nil, 0) == 0,
                  count >= 0, count <= maximumBytes else { return nil }
            var data = Data(count: min(maximumBytes, count + 16_384))
            var readCount = data.count
            let result = data.withUnsafeMutableBytes { buffer in
                sysctl(&mib, u_int(mib.count), buffer.baseAddress, &readCount, nil, 0)
            }
            if result != 0 {
                if errno == ENOMEM { continue }
                return nil
            }
            guard readCount <= data.count else { return nil }
            data.count = readCount
            guard let routes = decode(data) else { return nil }
            return classify(routes: routes, interfaces: interfaces, physicalInterfaces: physicalInterfaces)
        }
        return nil
    }
}
#endif
