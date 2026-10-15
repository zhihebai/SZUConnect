import Foundation
#if os(macOS)
import Darwin
#endif

@main
struct NetworkIdentityTests {
    static func main() {
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        let baseline: [String: [String]] = [
            "interfaces": ["fixture-en0", "fixture-en1"],
            "services": ["fixture-service-A", "fixture-service-B"],
            "primaryService": ["fixture-service-A"], "primaryInterface": ["fixture-en0"],
            "pathGateways": ["192.0.2.1"],
            "routers": ["192.0.2.1"], "addresses": ["192.0.2.2", "192.0.2.3"],
            "masks": ["255.255.255.0"], "attachment": ["fixture-attachment"]
        ]
        let identity = NetworkIdentity.make(components: baseline)
        check(identity != nil, "Complete continuity evidence produces an identity")
        check(identity?.permitsBriefInterruption == true,
              "Nonempty attachment evidence permits a brief continuity interruption")
        for attachment: [String]? in [nil, [], [""]] {
            var weak = baseline
            weak["attachment"] = attachment
            let weakIdentity = NetworkIdentity.make(components: weak)
            check(weakIdentity != nil && weakIdentity?.permitsBriefInterruption == false,
                  "Missing, empty or blank attachment evidence never permits cross-interruption trust")
        }
        check(!NetworkIdentity(fingerprint: "A").permitsBriefInterruption,
              "Opaque identity defaults to no cross-interruption permission")
        check(identity == NetworkIdentity.make(components: baseline), "Repeated evidence has stable identity")
        var reordered = baseline
        reordered["addresses"] = ["192.0.2.3", "192.0.2.2", "192.0.2.2", ""]
        reordered["routers"] = ["192.0.2.1", "192.0.2.1"]
        check(identity == NetworkIdentity.make(components: reordered),
              "Order, duplicate and empty values do not revoke identity continuity")
        for component in ["interfaces", "services", "routers", "addresses", "masks", "attachment"] {
            var changed = baseline
            changed[component] = ["different-fixture-value"]
            check(identity != NetworkIdentity.make(components: changed),
                  "Changed \(component) changes network identity")
        }
        for (component, replacement) in [
            ("primaryService", "fixture-service-B"),
            ("primaryInterface", "fixture-en1"),
            ("pathGateways", "192.0.2.254")
        ] {
            var changed = baseline
            changed[component] = [replacement]
            check(identity != NetworkIdentity.make(components: changed),
                  "Changed \(component) changes identity even when all interfaces and services remain present")
        }
        var noPathGateway = baseline
        noPathGateway["pathGateways"] = []
        check(NetworkIdentity.make(components: noPathGateway) != nil,
              "A known primary route remains identifiable when path gateway metadata is empty")
        noPathGateway.removeValue(forKey: "pathGateways")
        check(NetworkIdentity.make(components: noPathGateway) != nil,
              "System configuration router evidence supports identity without optional path gateways")
        for required in ["interfaces", "services", "routers", "primaryService", "primaryInterface"] {
            var missing = baseline
            missing.removeValue(forKey: required)
            check(NetworkIdentity.make(components: missing) == nil, "Missing \(required) cannot imply a known network")
            missing[required] = []
            check(NetworkIdentity.make(components: missing) == nil, "Empty \(required) cannot imply a known network")
            missing[required] = [""]
            check(NetworkIdentity.make(components: missing) == nil, "Blank \(required) cannot imply a known network")
        }
        check(NetworkIdentity.make(components: [:]) == nil, "No continuity evidence cannot authorize a network")
        check(NetworkIdentity(fingerprint: "A") == NetworkIdentity(fingerprint: "A")
              && NetworkIdentity(fingerprint: "A") != NetworkIdentity(fingerprint: "B"),
              "Opaque test identities retain equality without any raw network metadata")

        // A VPN/TUN tunnel may own the default route; the physical attachment is still chosen
        // deterministically, and several unordered candidates are never merged or guessed.
        typealias Snapshot = NetworkContinuitySnapshot
        typealias Selection = NetworkContinuitySnapshot.PhysicalSelection
        let wifi = Snapshot.ServiceState(interface: "fixture-en0", hasRouter: true)
        let wired = Snapshot.ServiceState(interface: "fixture-en5", hasRouter: true)
        let tunnel = Snapshot.ServiceState(interface: "fixture-utun4", hasRouter: true)
        let physical: Set<String> = ["fixture-en0", "fixture-en5"]
        let services = ["S-wifi": wifi, "S-wired": wired, "S-vpn": tunnel]
        check(Snapshot.selectPhysicalService(primaryService: "S-wifi", primaryInterface: "fixture-en0",
                                             serviceOrder: ["S-wired", "S-wifi", "S-vpn"], services: services,
                                             physicalInterfaces: physical)
              == Selection(service: "S-wifi", interface: "fixture-en0", viaTunnel: false),
              "A physical primary service is used as-is, regardless of service order")
        check(Snapshot.selectPhysicalService(primaryService: "S-wifi", primaryInterface: "fixture-en0", serviceOrder: [],
                                             services: ["S-wifi": Snapshot.ServiceState(interface: "fixture-en0", hasRouter: false)],
                                             physicalInterfaces: physical) == nil,
              "A physical primary without a router is not an attachment")
        check(Snapshot.selectPhysicalService(primaryService: "S-wifi", primaryInterface: "fixture-en5", serviceOrder: [],
                                             services: services, physicalInterfaces: physical) == nil,
              "A primary service that disagrees with its interface is rejected")
        check(Snapshot.selectPhysicalService(primaryService: "S-vpn", primaryInterface: "fixture-utun4",
                                             serviceOrder: ["S-vpn", "S-wired", "S-wifi"], services: services,
                                             physicalInterfaces: physical)
              == nil,
              "Service preference alone cannot establish a tunnel's egress when multiple links are active")
        check(Snapshot.selectPhysicalService(primaryService: "S-vpn", primaryInterface: "fixture-utun4", serviceOrder: [],
                                             services: ["S-wifi": wifi, "S-vpn": tunnel], physicalInterfaces: physical)
              == Selection(service: "S-wifi", interface: "fixture-en0", viaTunnel: true),
              "Without a service order a single physical candidate is still usable")
        check(Snapshot.selectPhysicalService(primaryService: "S-vpn", primaryInterface: "fixture-utun4", serviceOrder: [],
                                             services: services, physicalInterfaces: physical) == nil,
              "Several unordered physical services are never merged or guessed")
        check(Snapshot.selectPhysicalService(primaryService: "S-vpn", primaryInterface: "fixture-utun4",
                                             serviceOrder: ["S-vpn", "S-wifi"],
                                             services: ["S-vpn": tunnel,
                                                        "S-wifi": Snapshot.ServiceState(interface: "fixture-en0", hasRouter: false)],
                                             physicalInterfaces: physical) == nil,
              "A tunnel over a physical service without a router yields no identity")
        check(Snapshot.selectPhysicalService(primaryService: nil, primaryInterface: nil,
                                             serviceOrder: ["S-wifi"], services: services, physicalInterfaces: physical) == nil,
              "No IPv4 primary route means no attachment")
        var direct = baseline
        direct["route"] = ["direct"]
        var tunnelled = baseline
        tunnelled["route"] = ["tunnel", "S-vpn", "fixture-utun4"]
        check(NetworkIdentity.make(components: direct) != nil
              && NetworkIdentity.make(components: direct) != NetworkIdentity.make(components: tunnelled),
              "Turning a tunnel on or off changes the identity, so trust must be re-confirmed")
        let snapshot = NetworkContinuitySnapshot(identity: nil, linkActive: false)
        check(!snapshot.tunnel && snapshot.physicalLabels.isEmpty, "Snapshot defaults claim no tunnel and no labels")
        let inactiveWired = Snapshot.ServiceState(interface: "fixture-en5", hasRouter: true, linkActive: false)
        check(Snapshot.selectPhysicalService(primaryService: "S-vpn", primaryInterface: "fixture-utun4",
              serviceOrder: ["S-wired", "S-wifi"], services: ["S-vpn": tunnel, "S-wired": inactiveWired, "S-wifi": wifi],
              physicalInterfaces: physical) == Selection(service: "S-wifi", interface: "fixture-en0", viaTunnel: true),
              "Stale configured router on an inactive preferred link does not hide the live fallback")
        check(Snapshot.selectPhysicalService(primaryService: "S-wired", primaryInterface: "fixture-en5",
              serviceOrder: [], services: ["S-wired": inactiveWired], physicalInterfaces: physical) == nil,
              "An inactive direct primary cannot establish trust")
        check(Snapshot.stableInterfaceComponent("en0", kind: "wifi") == Snapshot.stableInterfaceComponent("en0", kind: "Wi-Fi"),
              "Visible NWPath and hidden SystemConfiguration Wi-Fi representations match")
        check(Snapshot.stableInterfaceComponent("en5", kind: "wiredEthernet") == Snapshot.stableInterfaceComponent("en5", kind: "有线"),
              "Visible and hidden Ethernet representations match")
        check(Snapshot.stableInterfaceComponent("en0", kind: "wifi") != Snapshot.stableInterfaceComponent("en1", kind: "wifi"),
              "Different BSD attachments remain distinguishable")
        check(PathObservation.permitsProbe(pathSatisfied: true, physicalPath: false, tunnelUsable: true),
              "A live tunnel may be probed without pretending its physical trust is unambiguous")
        check(!PathObservation.permitsProbe(pathSatisfied: false, physicalPath: true, tunnelUsable: true),
              "An unsatisfied path cannot be probed based only on stale link metadata")
        check(!PathObservation.permitsProbe(pathSatisfied: true, physicalPath: false, tunnelUsable: false),
              "No known physical or tunnel attachment stays unavailable")
        let wiredMemory = NetworkDescriptor(kind: "ethernet", raw: ["gwip": "192.0.2.1", "net": "192.0.2.0/24"])
        let wifiMemory = NetworkDescriptor(kind: "wifi", raw: ["gwip": "198.51.100.1", "net": "198.51.100.0/24"])
        let memoryByService = ["S-wired": wiredMemory, "S-wifi": wifiMemory]
        check(Snapshot.memoryDescriptors(for: Selection(service: "S-wired", interface: "fixture-en5", viaTunnel: false),
              byService: memoryByService) == [wiredMemory], "Only the selected wired egress contributes memory eligibility")
        check(Snapshot.memoryDescriptors(for: Selection(service: "S-wifi", interface: "fixture-en0", viaTunnel: false),
              byService: memoryByService) == [wifiMemory], "Switching to Wi-Fi selects its own memory rather than the idle cable")
        check(Snapshot.memoryDescriptors(for: nil, byService: memoryByService).isEmpty,
              "An ambiguous egress cannot borrow memory from an attached interface")
        check(Snapshot.memoryDescriptors(for: Selection(service: "missing", interface: "fixture-en0", viaTunnel: true),
              byService: memoryByService).isEmpty, "Missing selected service evidence grants no memory eligibility")
        #if os(macOS)
        typealias Routes = TunnelRouteSnapshot
        let routeInterfaces: [UInt16: Routes.Interface] = [
            7: .init(name: "utun13", up: true, running: true),
            8: .init(name: "en0", up: true, running: true)
        ]
        func v4(_ destination: [UInt8] = [0, 0, 0, 0], prefix: Int = 0,
                flags: Int32 = RTF_UP | RTF_GATEWAY, index: UInt16 = 7,
                gateway: [UInt8] = [172, 19, 0, 1]) -> Routes.Route {
            .init(family: AF_INET, destination: destination, prefixLength: prefix,
                  gateway: gateway, interfaceIndex: index, flags: flags)
        }
        func classify(_ routes: [Routes.Route], interfaces: [UInt16: Routes.Interface] = routeInterfaces) -> Routes {
            Routes.classify(routes: routes, interfaces: interfaces)
        }
        let split = [v4(prefix: 1), v4([128, 0, 0, 0], prefix: 1)]
        check(classify(split).hasTunnelRoutes, "Split IPv4 defaults reveal a TUN while SC primary may remain physical")
        check(classify([v4()]).hasTunnelRoutes, "An unscoped usable tunnel default is detected")
        check(classify([v4([8, 0, 0, 0], prefix: 5)]).hasTunnelRoutes,
              "A more-specific public route reveals partial tunnel routing")
        let ipv6 = Routes.Route(family: AF_INET6, destination: [0x20] + Array(repeating: 0, count: 15),
                                prefixLength: 3, gateway: [0xfd, 0], interfaceIndex: 7, flags: RTF_UP | RTF_GATEWAY)
        check(classify([ipv6]).hasTunnelRoutes, "IPv6 global-unicast capture is detected independently of IPv4")
        check(!classify([]).hasTunnelRoutes, "A running but idle utun interface is not tunnel-routing evidence")
        let ipv6ScopedDefault = Routes.Route(family: AF_INET6, destination: Array(repeating: 0, count: 16),
                                            prefixLength: 0, gateway: [0xfe, 0x80], interfaceIndex: 7,
                                            flags: RTF_UP | RTF_GATEWAY | RTF_IFSCOPE)
        check(!classify([ipv6ScopedDefault]).hasTunnelRoutes,
              "Idle macOS interface-scoped IPv6 defaults do not imply a selected VPN")
        for flags: Int32 in [RTF_GATEWAY, RTF_UP | RTF_GATEWAY | RTF_REJECT,
                            RTF_UP | RTF_GATEWAY | RTF_BLACKHOLE, RTF_UP | RTF_GATEWAY | RTF_DEAD,
                            RTF_UP | RTF_GATEWAY | RTF_WASCLONED, RTF_UP | RTF_GATEWAY | RTF_IFSCOPE] {
            check(!classify([v4(flags: flags)]).hasTunnelRoutes,
                  "Inactive, rejected, discarded, scoped-default and cloned route flags cannot establish tunnel evidence: \(flags)")
        }
        check(!classify([v4()], interfaces: [7: .init(name: "utun13", up: false, running: true)]).hasTunnelRoutes,
              "An interface that is down cannot establish tunnel evidence")
        check(!classify([v4()], interfaces: [7: .init(name: "utun13", up: true, running: false)]).hasTunnelRoutes,
              "An interface that is not running cannot establish tunnel evidence")
        check(!classify([v4(index: 8)]).hasTunnelRoutes, "Ordinary physical routes are not called VPN routes")
        check(!classify([v4([127, 0, 0, 0], prefix: 8), v4([169, 254, 0, 0], prefix: 16),
                         v4([224, 0, 0, 0], prefix: 4)]).hasTunnelRoutes,
              "Loopback, link-local and multicast routes do not establish VPN traffic capture")
        check(classify(split) == classify(Array(split.reversed()) + split),
              "Route order and duplicate notifications preserve the identity evidence")
        check(classify(split) == classify(split + [v4([8, 8, 8, 8], prefix: 32, flags: RTF_UP | RTF_GATEWAY | RTF_WASCLONED)]),
              "A transient host clone caused by a request never revokes network trust")
        check(classify([v4()]) != classify([v4(gateway: [172, 19, 0, 2])]),
              "A changed explicit gateway changes the stable routing evidence")
        var routeIdentity = baseline
        routeIdentity["route"] = classify(split).identityComponents
        let routeID = NetworkIdentity.make(components: routeIdentity)
        routeIdentity["route"] = classify([v4()]).identityComponents
        check(routeID != NetworkIdentity.make(components: routeIdentity),
              "A changed route prefix changes the ephemeral network identity")
        check(Snapshot.selectPhysicalService(primaryService: "S-wifi", primaryInterface: "fixture-en0", serviceOrder: [],
                                             services: services, physicalInterfaces: physical, routingTunnel: true) == nil,
              "Split tunnel evidence retains the strict multiple-physical-link trust rule")
        check(Snapshot.selectPhysicalService(primaryService: "S-wifi", primaryInterface: "fixture-en0", serviceOrder: [],
                                             services: ["S-wifi": wifi], physicalInterfaces: physical, routingTunnel: true)
              == Selection(service: "S-wifi", interface: "fixture-en0", viaTunnel: true),
              "A split tunnel may retain a single proven physical attachment")

        check(classify([v4(flags: RTF_UP, gateway: [])]).hasTunnelRoutes,
              "A live unscoped TUN default interface route does not need the gateway flag")
        check(classify([v4(prefix: 1, flags: RTF_UP, gateway: []),
                        v4([128, 0, 0, 0], prefix: 1, flags: RTF_UP, gateway: [])]).identityComponents.count == 2,
              "Both halves of split-default interface routes are recognized without a gateway flag")
        check(classify([v4([8, 8, 8, 0], prefix: 24, flags: RTF_UP, gateway: [])]).hasTunnelRoutes,
              "An unscoped public split interface route is detected without a gateway flag")
        check(classify([v4([8, 8, 8, 0], prefix: 24, flags: RTF_UP | RTF_IFSCOPE, gateway: [])]).hasTunnelRoutes,
              "A scoped public split interface route counts as configured tunnel routing")
        check(classify([v4([8, 8, 8, 8], prefix: 32, flags: RTF_UP | RTF_GATEWAY | RTF_HOST | RTF_IFSCOPE)]).hasTunnelRoutes,
              "An explicit scoped public host route is detected when it is not a clone")
        let ipv6Interface = Routes.Route(family: AF_INET6, destination: ipv6.destination, prefixLength: 3,
                                         gateway: [], interfaceIndex: 7, flags: RTF_UP)
        check(classify([ipv6Interface]).hasTunnelRoutes,
              "IPv6 global-unicast interface routing is detected without a gateway flag")
        let ipv6ScopedSplit = Routes.Route(family: AF_INET6, destination: [0x26, 0x06] + Array(repeating: 0, count: 14),
                                          prefixLength: 32, gateway: [], interfaceIndex: 7, flags: RTF_UP | RTF_IFSCOPE)
        check(classify([ipv6ScopedSplit]).hasTunnelRoutes,
              "A scoped public IPv6 split route is detected without treating all scoped defaults as active")
        for flags: Int32 in [RTF_UP, RTF_UP | RTF_IFSCOPE, RTF_UP | RTF_GATEWAY | RTF_IFSCOPE] {
            check(!classify([v4([172, 19, 0, 0], prefix: 30, flags: flags)]).hasTunnelRoutes,
                  "Private point-to-point attachment does not create positive evidence from interface/scoped routing: \(flags)")
        }
        check(!classify([v4(flags: RTF_UP | RTF_IFSCOPE, gateway: [])]).hasTunnelRoutes,
              "An idle IPv4 scoped default without a gateway flag is still ignored")
        check(!classify([v4([127, 0, 0, 1], prefix: 32, flags: RTF_UP | RTF_IFSCOPE),
                         v4([169, 254, 0, 0], prefix: 16, flags: RTF_UP | RTF_IFSCOPE),
                         v4([224, 0, 0, 0], prefix: 4, flags: RTF_UP | RTF_IFSCOPE),
                         v4([192, 0, 2, 0], prefix: 24, flags: RTF_UP | RTF_IFSCOPE)]).hasTunnelRoutes,
              "Scoped local and documentation targets are not public split-route evidence")
        let privateIPv6 = Routes.Route(family: AF_INET6, destination: [0xfd, 0] + Array(repeating: 0, count: 14),
                                       prefixLength: 126, gateway: [], interfaceIndex: 7, flags: RTF_UP | RTF_IFSCOPE)
        check(!classify([privateIPv6]).hasTunnelRoutes, "A scoped IPv6 ULA attachment does not imply active VPN traffic")
        check(classify([v4([8, 8, 8, 0], prefix: 24)])
              != classify([v4([8, 8, 8, 0], prefix: 24, flags: RTF_UP | RTF_GATEWAY | RTF_IFSCOPE)]),
              "Changing a configured route's interface scope changes its identity evidence")
        check(classify([v4([8, 8, 8, 0], prefix: 24, flags: RTF_UP | RTF_IFSCOPE)])
              == classify([v4([8, 8, 8, 0], prefix: 24, flags: RTF_UP | RTF_IFSCOPE),
                           v4([8, 8, 8, 8], prefix: 32, flags: RTF_UP | RTF_IFSCOPE | RTF_WASCLONED)]),
              "A scoped temporary host clone does not change stable split-route evidence")

        func socketV4(_ value: [UInt8]) -> [UInt8] { [16, UInt8(AF_INET), 0, 0] + value + Array(repeating: 0, count: 8) }
        func routeMessage(destination: [UInt8] = [8, 0, 0, 0], mask: [UInt8] = [248, 0, 0, 0],
                          version: UInt8 = UInt8(RTM_VERSION), addrs: Int32 = RTA_DST | RTA_GATEWAY | RTA_NETMASK) -> Data {
            var header = rt_msghdr2()
            header.rtm_msglen = UInt16(MemoryLayout<rt_msghdr2>.size + 48)
            header.rtm_version = version
            header.rtm_type = UInt8(RTM_GET2)
            header.rtm_index = 7
            header.rtm_flags = RTF_UP | RTF_GATEWAY
            header.rtm_addrs = addrs
            return withUnsafeBytes(of: &header) { Data($0) }
                + Data(socketV4(destination) + socketV4([172, 19, 0, 1]) + socketV4(mask))
        }
        let message = routeMessage()
        let decoded = Routes.decode(message)
        check(decoded?.count == 1 && decoded?.first?.prefixLength == 5 && classify(decoded ?? []).hasTunnelRoutes,
              "Darwin route messages are decoded into real prefix and interface evidence")
        check(Routes.decode(message + message)?.count == 2, "Several variable messages advance by their validated lengths")
        check(Routes.decode(Data()) == [], "An empty valid route dump contains no evidence")
        check((1..<message.count).allSatisfy { Routes.decode(message.prefix($0)) == nil },
              "Every truncated route-buffer prefix fails safely without partial evidence")
        var zeroLength = message
        zeroLength[0] = 0; zeroLength[1] = 0
        check(Routes.decode(zeroLength) == nil, "Zero-length route messages cannot loop forever")
        var invalidSocket = message
        invalidSocket[MemoryLayout<rt_msghdr2>.size] = 255
        check(Routes.decode(invalidSocket) == nil, "A sockaddr cannot overrun its route message")
        check(Routes.decode(routeMessage(version: 0)) == nil, "Unknown route-message versions fail safely")
        check(Routes.decode(routeMessage(addrs: Int32.max)) == nil, "Unknown address bits fail safely")
        check(Routes.decode(routeMessage(mask: [255, 0, 255, 0])) == nil, "Noncontiguous route masks are rejected")
        check(Routes.decode(Data(repeating: 0, count: Routes.maximumBytes + 1)) == nil,
              "Excessive route buffers are rejected before parsing")
        var compactMask = message
        compactMask.removeLast(16)
        compactMask.append(contentsOf: [0, 0, 0, 0])
        compactMask[0] = UInt8(compactMask.count & 255)
        compactMask[1] = UInt8(compactMask.count >> 8)
        for i in 0..<4 { compactMask[MemoryLayout<rt_msghdr2>.size + 4 + i] = 0 }
        check(Routes.decode(compactMask)?.first?.prefixLength == 0,
              "Darwin's zero-length default mask uses its required four-byte alignment")

        // A system physical default can support explicit session trust under a TUN, but
        // cannot establish which physical interface the tunnel's own outbound socket uses.
        typealias PhysicalDefault = Snapshot.PhysicalDefault
        let defaultInterfaces: [UInt16: Routes.Interface] = [
            7: .init(name: "utun13", up: true, running: true),
            8: .init(name: "fixture-en0", up: true, running: true),
            9: .init(name: "fixture-en5", up: true, running: true)
        ]
        let defaultWired = v4(index: 9, gateway: socketV4([192, 0, 2, 1]))
        let defaultWiFi = v4(index: 8, gateway: socketV4([198, 51, 100, 1]))
        let wiredDefault = PhysicalDefault(interface: "fixture-en5", gateway: "192.0.2.1")
        func defaults(_ routes: [Routes.Route], interfaces: [UInt16: Routes.Interface] = defaultInterfaces) -> [PhysicalDefault] {
            Routes.physicalIPv4Defaults(routes: routes, interfaces: interfaces, physicalInterfaces: physical)
        }
        check(defaults([defaultWired]) == [wiredDefault],
              "An active unscoped physical IPv4 default retains its decoded gateway")
        check(defaults(Routes.decode(routeMessage(destination: [0, 0, 0, 0], mask: [0, 0, 0, 0])) ?? [],
                       interfaces: [7: .init(name: "fixture-en5", up: true, running: true)])
              == [PhysicalDefault(interface: "fixture-en5", gateway: "172.19.0.1")],
              "A decoded Darwin default preserves the sockaddr IPv4 gateway used by SC matching")
        check(defaults([defaultWired, defaultWired]) == [wiredDefault],
              "Duplicate physical-default records do not invent a second exit")
        check(defaults([defaultWired, defaultWiFi]).count == 2,
              "Two distinct physical defaults retain the ambiguity instead of choosing an order")
        check(defaults([v4(gateway: socketV4([192, 0, 2, 1]))]).isEmpty,
              "A tunnel default cannot become physical-default authority")
        for flags: Int32 in [RTF_GATEWAY, RTF_UP, RTF_UP | RTF_GATEWAY | RTF_IFSCOPE,
                            RTF_UP | RTF_GATEWAY | RTF_REJECT, RTF_UP | RTF_GATEWAY | RTF_BLACKHOLE,
                            RTF_UP | RTF_GATEWAY | RTF_WASCLONED, RTF_UP | RTF_GATEWAY | RTF_LOCAL,
                            RTF_UP | RTF_GATEWAY | RTF_DEAD, RTF_UP | RTF_GATEWAY | RTF_CONDEMNED,
                            RTF_UP | RTF_GATEWAY | RTF_LLINFO, RTF_UP | RTF_GATEWAY | RTF_MULTICAST,
                            RTF_UP | RTF_GATEWAY | RTF_BROADCAST, RTF_UP | RTF_GATEWAY | RTF_HOST] {
            check(defaults([v4(flags: flags, index: 9, gateway: socketV4([192, 0, 2, 1]))]).isEmpty,
                  "Unusable, scoped or transient physical defaults supply no trust evidence: \(flags)")
        }
        check(defaults([v4([192, 0, 2, 0], prefix: 24, index: 9, gateway: socketV4([192, 0, 2, 1]))]).isEmpty,
              "A physical split route is not a physical default")
        check(defaults([v4(index: 9, gateway: [4, UInt8(AF_LINK), 0, 0])]).isEmpty,
              "A link-layer gateway cannot be guessed into an IPv4 router")
        check(defaults([v4(index: 9, gateway: [8, UInt8(AF_INET), 0, 0, 192, 0, 2, 1])]).isEmpty
              && defaults([v4(index: 9, gateway: socketV4([0, 0, 0, 0]))]).isEmpty,
              "Truncated and unspecified IPv4 gateways supply no physical-default authority")
        check(defaults([defaultWired], interfaces: [9: .init(name: "fixture-en5", up: false, running: true)]).isEmpty
              && defaults([defaultWired], interfaces: [9: .init(name: "fixture-en5", up: true, running: false)]).isEmpty,
              "A down or non-running physical interface supplies no default authority")
        let snapshotWithDefault = Routes.classify(routes: split + [defaultWired], interfaces: defaultInterfaces,
                                                 physicalInterfaces: physical)
        check(snapshotWithDefault.hasTunnelRoutes && snapshotWithDefault.physicalDefaults == [wiredDefault],
              "Physical-default extraction preserves independent active TUN routing evidence")
        let scopedWiFiDefault = v4(flags: RTF_UP | RTF_GATEWAY | RTF_IFSCOPE, index: 8,
            gateway: socketV4([198, 51, 100, 1]))
        let snapshotWithScoped = Routes.classify(routes: split + [defaultWired, scopedWiFiDefault],
            interfaces: defaultInterfaces, physicalInterfaces: physical)
        check(snapshotWithScoped.physicalDefaults == [wiredDefault],
              "Secondary scoped default never changes system-primary default semantics")
        check(snapshotWithScoped.boundPhysicalDefaults == [
            PhysicalDefault(interface: "fixture-en0", gateway: "198.51.100.1"), wiredDefault],
              "Interface-bound candidates retain verified scoped and unscoped physical defaults")
        check(snapshotWithScoped.identityComponents == snapshotWithDefault.identityComponents,
              "A physical scoped default cannot become virtual tunnel evidence")
        for flags: Int32 in [RTF_GATEWAY | RTF_IFSCOPE, RTF_UP | RTF_IFSCOPE,
                            RTF_UP | RTF_GATEWAY | RTF_IFSCOPE | RTF_REJECT,
                            RTF_UP | RTF_GATEWAY | RTF_IFSCOPE | RTF_BLACKHOLE,
                            RTF_UP | RTF_GATEWAY | RTF_IFSCOPE | RTF_WASCLONED,
                            RTF_UP | RTF_GATEWAY | RTF_IFSCOPE | RTF_HOST,
                            RTF_UP | RTF_GATEWAY | RTF_IFSCOPE | RTF_LOCAL,
                            RTF_UP | RTF_GATEWAY | RTF_IFSCOPE | RTF_DEAD] {
            let scoped = Routes.classify(routes: [v4(flags: flags, index: 8, gateway: socketV4([198, 51, 100, 1]))],
                interfaces: defaultInterfaces, physicalInterfaces: physical)
            check(scoped.boundPhysicalDefaults.isEmpty, "Invalid scoped default cannot supply bound route authority: \(flags)")
        }
        check(Routes.classify(routes: [scopedWiFiDefault], interfaces: [8: .init(name: "fixture-en0", up: true, running: false)],
            physicalInterfaces: physical).boundPhysicalDefaults.isEmpty,
              "Secondary scoped route still requires a running physical interface")
        check(Routes.classify(routes: [v4(index: 7, gateway: socketV4([192, 0, 2, 1]))], interfaces: defaultInterfaces,
            physicalInterfaces: physical).boundPhysicalDefaults.isEmpty,
              "Virtual tunnel routes never supply physical candidate defaults")
        let provenServices = [
            "S-wifi": Snapshot.ServiceState(interface: "fixture-en0", hasRouter: true, router: "198.51.100.1"),
            "S-wired": Snapshot.ServiceState(interface: "fixture-en5", hasRouter: true, router: "192.0.2.1")
        ]
        func selectProven(_ defaults: [PhysicalDefault], primary: String = "S-wired", interface: String = "fixture-en5",
                          services: [String: Snapshot.ServiceState] = provenServices) -> Selection? {
            Snapshot.selectPhysicalService(primaryService: primary, primaryInterface: interface, serviceOrder: [],
                                           services: services, physicalInterfaces: physical, routingTunnel: true,
                                           physicalDefaults: defaults)
        }
        let compositeSelection = Selection(service: "S-wired", interface: "fixture-en5", viaTunnel: true,
                                           requiresCompositeTrust: true)
        check(selectProven([wiredDefault]) == compositeSelection,
              "Matching SC physical primary and unique physical default permit explicit composite session trust")
        check(selectProven([]) == nil && selectProven(defaults([defaultWired, defaultWiFi])) == nil,
              "Missing or competing physical defaults cannot authorize composite trust")
        check(selectProven([PhysicalDefault(interface: "fixture-en5", gateway: "192.0.2.254")]) == nil,
              "A route gateway that disagrees with the selected service router rejects composite trust")
        check(selectProven([wiredDefault], primary: "S-wifi", interface: "fixture-en0") == nil
              && selectProven([wiredDefault], primary: "S-wired", interface: "fixture-en0") == nil,
              "Route, primary service and primary interface must agree")
        check(Snapshot.memoryDescriptors(for: compositeSelection, byService: memoryByService).isEmpty,
              "Composite session trust cannot inherit a selected network's persistent memory")

        let physicalDetails: [String: [String: Any]] = [
            "S-wired": ["InterfaceName": "fixture-en5", "Router": "192.0.2.1", "Addresses": ["192.0.2.4"],
                        "SubnetMasks": ["255.255.255.0"], "NetworkSignature": "fixture-wired"],
            "S-wifi": ["InterfaceName": "fixture-en0", "Router": "198.51.100.1", "Addresses": ["198.51.100.4", "198.51.100.5"],
                       "SubnetMasks": ["255.255.255.0"], "NetworkSignature": "fixture-wifi"]
        ]
        let attachments = Snapshot.compositeAttachmentComponents(services: provenServices, details: physicalDetails,
                                                                physicalInterfaces: physical)
        check(attachments?.count == 2, "Composite identity retains every live physical attachment as an associated record")
        func compositeIdentity(_ details: [String: [String: Any]], services: [String: Snapshot.ServiceState] = provenServices) -> NetworkIdentity? {
            guard let components = Snapshot.compositeAttachmentComponents(services: services, details: details,
                                                                         physicalInterfaces: physical) else { return nil }
            var all = baseline
            all["physicalAttachments"] = components
            return NetworkIdentity.make(components: all)
        }
        let compositeID = compositeIdentity(physicalDetails)
        check(compositeID != nil, "Complete composite attachment evidence yields a RAM-only identity")
        for (key, replacement) in [("Addresses", ["198.51.100.5"]), ("SubnetMasks", ["255.255.0.0"]),
                                   ("NetworkSignature", "fixture-other-wifi") ] as [(String, Any)] {
            var changed = physicalDetails
            changed["S-wifi"]?[key] = replacement
            check(compositeID != compositeIdentity(changed),
                  "Changing non-primary Wi-Fi \(key) revokes composite session identity")
        }
        var reorderedDetails = physicalDetails
        reorderedDetails["S-wifi"]?["Addresses"] = ["198.51.100.5", "198.51.100.4", "198.51.100.4"]
        check(compositeID == compositeIdentity(reorderedDetails),
              "Reordered and duplicate attachment values preserve composite identity")
        var pairedDetails = physicalDetails
        pairedDetails["S-wifi"]?["SubnetMasks"] = ["255.255.255.0", "255.255.0.0"]
        var swappedMasks = pairedDetails
        swappedMasks["S-wifi"]?["SubnetMasks"] = ["255.255.0.0", "255.255.255.0"]
        check(compositeIdentity(pairedDetails) != compositeIdentity(swappedMasks),
              "Changing address-mask association within one service revokes the composite identity")
        var mismatchedMasks = physicalDetails
        mismatchedMasks["S-wifi"]?["SubnetMasks"] = ["255.255.255.0", "255.255.0.0", "255.0.0.0"]
        check(compositeIdentity(mismatchedMasks) == nil,
              "Inconsistent address-mask evidence cannot identify a composite attachment")
        var swappedDetails = physicalDetails
        swappedDetails["S-wifi"]?["Addresses"] = physicalDetails["S-wired"]?["Addresses"]
        swappedDetails["S-wired"]?["Addresses"] = physicalDetails["S-wifi"]?["Addresses"]
        check(compositeID != compositeIdentity(swappedDetails),
              "Swapping addresses between services changes identity even when the combined address set is unchanged")
        var changedRouterDetails = physicalDetails
        var changedRouterServices = provenServices
        changedRouterDetails["S-wifi"]?["Router"] = "198.51.100.254"
        changedRouterServices["S-wifi"]?.router = "198.51.100.254"
        check(compositeID != compositeIdentity(changedRouterDetails, services: changedRouterServices),
              "Changing a secondary service's associated router revokes composite identity")
        var displayOnlyDetails = physicalDetails
        displayOnlyDetails["S-wifi"]?["SSID"] = "fixture-display-only-name"
        check(compositeID == compositeIdentity(displayOnlyDetails),
              "Display-only Wi-Fi names do not enter composite trust identity")
        var missingDetails = physicalDetails
        missingDetails.removeValue(forKey: "S-wifi")
        check(compositeIdentity(missingDetails) == nil,
              "Missing evidence for a live secondary attachment rejects composite identity")
        var changedServices = provenServices
        changedServices["S-wifi"]?.linkActive = false
        check(compositeID != compositeIdentity(physicalDetails, services: changedServices),
              "Removing a secondary physical attachment revokes composite session identity")
        #endif
        print("\(passed) pure network-identity assertions passed. No system snapshot, app, network or persisted identifier was used.")
    }
}
