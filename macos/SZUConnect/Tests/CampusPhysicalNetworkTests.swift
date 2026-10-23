import Foundation
import Darwin

@main
struct CampusPhysicalNetworkTests {
    static var checks = 0
    static func check(_ condition: @autoclosure () -> Bool, _ label: String) {
        checks += 1
        if !condition() { fatalError("FAIL: \(label)") }
    }
    static func rejects(_ data: Data, host: String = "net.szu.edu.cn", transaction: UInt16 = 0x1234, _ label: String) {
        do { _ = try CampusPhysicalDNS.parse(data, host: host, transaction: transaction); check(false, label) }
        catch { check(error as? CampusPhysicalDNSError == .invalidResponse, label) }
    }
    static func name(_ host: String) -> [UInt8] {
        host.split(separator: ".").flatMap { [UInt8($0.utf8.count)] + Array($0.utf8) } + [0]
    }
    static func word(_ n: Int) -> [UInt8] { [UInt8((n >> 8) & 255), UInt8(n & 255)] }
    static func answer(_ owner: [UInt8] = [0xc0, 0x0c], type: Int = 1, data: [UInt8]) -> [UInt8] {
        owner + word(type) + [0, 1, 0, 0, 0, 30] + word(data.count) + data
    }
    static func reply(_ answers: [[UInt8]], extra: [[UInt8]] = []) -> Data {
        Data([0x12, 0x34, 0x81, 0x80, 0, 1] + word(answers.count) + [0, 0] + word(extra.count)
            + name("net.szu.edu.cn") + [0, 1, 0, 1] + answers.flatMap { $0 } + extra.flatMap { $0 })
    }
    static func main() throws {
        typealias DNS = CampusPhysicalDNS
        let basic = reply([answer(data: [203, 0, 113, 20])])
        check((try? DNS.parse(basic, host: "net.szu.edu.cn", transaction: 0x1234)) == ["203.0.113.20"], "compressed A answer")
        let duplicates = reply([answer(data: [203, 0, 113, 20]), answer(data: [203, 0, 113, 20]), answer(data: [192, 0, 2, 8])])
        check((try? DNS.parse(duplicates, host: "net.szu.edu.cn", transaction: 0x1234)) == ["192.0.2.8", "203.0.113.20"], "deduplicated A set")
        let cn = "login.szu.edu.cn"
        let cname = reply([answer(type: 5, data: name(cn)), answer(name(cn), data: [172, 16, 1, 5])])
        check((try? DNS.parse(cname, host: "net.szu.edu.cn", transaction: 0x1234)) == ["172.16.1.5"], "in-answer CNAME chain")
        let unordered = reply([answer(name(cn), data: [172, 16, 1, 5]), answer(type: 5, data: name(cn))])
        check((try? DNS.parse(unordered, host: "net.szu.edu.cn", transaction: 0x1234)) == ["172.16.1.5"], "CNAME records need not be ordered")
        rejects(reply([answer(name("evil.example"), data: [203, 0, 113, 2])]), "unrelated answer cannot authorize host")
        rejects(reply([], extra: [answer(data: [203, 0, 113, 2])]), "additional A cannot supply identity")
        rejects(reply([answer(type: 5, data: name(cn))], extra: [answer(name(cn), data: [203, 0, 113, 2])]), "CNAME additional data not trusted")
        rejects(reply([answer(type: 5, data: name("net.szu.edu.cn"))]), "CNAME self loop")
        rejects(reply([answer(type: 5, data: name(cn)), answer(name(cn), type: 5, data: name("net.szu.edu.cn"))]), "CNAME two-node loop")
        rejects(reply([answer(type: 5, data: name(cn)), answer(type: 5, data: name("other.szu.edu.cn"))]), "ambiguous CNAME")
        rejects(reply([answer(type: 5, data: name(cn)), answer(data: [203, 0, 113, 20])]), "CNAME and A for same owner conflict")
        rejects(reply([answer(type: 5, data: name(cn) + [0])]), "trailing CNAME bytes rejected")
        rejects(basic, transaction: 0x4321, "transaction mismatch")
        rejects(basic, host: "www.apple.com", "question mismatch")
        rejects(basic, host: "evil.example", "host outside allowlist")
        for (index, byte, label) in [(2, UInt8(0x01), "query flags"), (2, UInt8(0x89), "unsupported opcode"),
                                    (2, UInt8(0x83), "truncated reply fails closed"), (3, UInt8(0x83), "NXDOMAIN"),
                                    (3, UInt8(0xc0), "reserved flag"), (5, UInt8(2), "multiple questions")] {
            var malformed = basic; malformed[index] = byte; rejects(malformed, label)
        }
        var incorrectType = basic
        let questionEnd = 12 + name("net.szu.edu.cn").count
        incorrectType[questionEnd + 1] = 28
        rejects(incorrectType, "AAAA response not an A question")
        var incorrectClass = basic; incorrectClass[questionEnd + 3] = 3
        rejects(incorrectClass, "question class mismatch")
        for size in 0..<basic.count { rejects(Data(basic.prefix(size)), "every truncation rejected \(size)") }
        rejects(basic + [0], "trailing packet data")
        rejects(reply([answer(data: [1, 2, 3])]), "A rdata must be four bytes")
        for address: [UInt8] in [[0, 1, 2, 3], [127, 0, 0, 1], [224, 0, 0, 1], [255, 255, 255, 255],
                                 [169, 254, 1, 1], [198, 18, 0, 1], [198, 19, 2, 3]] {
            rejects(reply([answer(data: address)]), "special/fake IP cannot supply host identity")
        }
        var selfPointer = basic
        let answerStart = questionEnd + 4
        selfPointer[answerStart] = 0xc0; selfPointer[answerStart + 1] = UInt8(answerStart)
        rejects(selfPointer, "compression self pointer")
        var forwardPointer = basic
        forwardPointer[answerStart] = 0xc0; forwardPointer[answerStart + 1] = UInt8(answerStart + 3)
        rejects(forwardPointer, "compression forward pointer")
        var headerPointer = basic; headerPointer[answerStart] = 0xc0; headerPointer[answerStart + 1] = 0
        rejects(headerPointer, "compression into header")
        var invalidLabel = basic; invalidLabel[12] = 0x40
        rejects(invalidLabel, "reserved label type")
        var excessive = basic; excessive[6] = 1; excessive[7] = 1
        rejects(excessive, "bounded record count")
        var cursor = 0
        do { _ = try DNS.decodeName([63] + Array(repeating: UInt8(97), count: 63) + [63]
            + Array(repeating: UInt8(98), count: 63) + [63] + Array(repeating: UInt8(99), count: 63)
            + [63] + Array(repeating: UInt8(100), count: 63) + [0], offset: &cursor)
            check(false, "expanded name bounded")
        } catch { check(true, "expanded name bounded") }
        for host in DNS.allowedHosts {
            let query = try DNS.query(host: host, transaction: 0x12ff)
            check(query.prefix(6) == Data([0x12, 0xff, 1, 0, 0, 1]), "query header")
            check(query.suffix(4) == Data([0, 1, 0, 1]), "query type and class")
        }
        do { _ = try DNS.query(host: "example.org", transaction: 0); check(false, "query host allowlist") }
        catch { check(error as? CampusPhysicalDNSError == .unavailable, "query host allowlist") }
        for address in ["1.2.3.4", "10.0.0.1", "172.30.255.42", "192.168.1.1", "223.255.255.255"] {
            check(DNS.isUsableIPv4(address), "unicast accepted")
        }
        for address in ["", "1.2.3", "256.1.1.1", "01.2.3.4", "1.2.3.4.", "1.2.3.4 ", "1.2.3.-1", "::1", "1.2.3.999"] {
            check(!DNS.isUsableIPv4(address), "malformed address rejected")
        }
        typealias Net = CampusPhysicalNetwork
        let physicalDefault = NetworkContinuitySnapshot.PhysicalDefault(interface: "fixture-en5", gateway: "192.168.20.1")
        let routes = TunnelRouteSnapshot(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault])
        func service(interface: String = "fixture-en5", router: String = "192.168.20.1", addresses: [String] = ["192.168.20.5"],
                     masks: [String] = ["255.255.255.0"], active: Bool = true, dhcp: [String] = ["192.168.20.1"],
                     dns: [String] = ["198.18.0.2"], evidence: String = "associated-record",
                     descriptor: NetworkDescriptor? = nil) -> Net.Service {
            Net.Service(interface: interface, router: router, addresses: addresses, masks: masks, linkActive: active,
                dhcpDNS: dhcp, serviceDNS: dns, evidence: evidence, descriptor: descriptor)
        }
        func evidence(primary: String? = "service-wired", name: String? = "fixture-en5", indices: [String: UInt32] = ["fixture-en5": 5],
                      live: [String: [String]] = ["fixture-en5": ["192.168.20.5"]], services: [String: Net.Service]? = nil,
                      routeSnapshot: TunnelRouteSnapshot? = nil, global: String = "primary-record") -> Net.Evidence {
            Net.Evidence(primaryService: primary, primaryInterface: name, indices: indices, liveAddresses: live,
                services: services ?? ["service-wired": service()], routes: routeSnapshot ?? routes, globalEvidence: global)
        }
        let good = Net.select(evidence())
        guard case .bound(let bound) = good else { fatalError("good physical evidence must bind") }
        check(bound.interfaceName == "fixture-en5" && bound.interfaceIndex == 5, "proven interface selected")
        check(bound.addresses == ["192.168.20.5"], "associated source address selected")
        check(bound.dnsServers == ["192.168.20.1"], "DHCP DNS precedes overwritten service DNS")
        check(bound.identity.count == 64 && !bound.identity.contains("192.168"), "identity hashed in RAM")
        check(Net.select(evidence()) == good, "canonical repeat stable")
        check(Net.selectStable(evidence(), evidence()) == good, "matching double snapshot binds")
        check(Net.selectStable(evidence(), evidence(global: "changed")) == good, "global metadata cannot change proven physical authority")
        check(Net.selectStable(evidence(), evidence(services: ["service-wired": service(evidence: "changed")])) == .unavailable,
              "raced associated service snapshot rejected")
        check(Net.selectStable(evidence(), evidence(routeSnapshot: .init(identityComponents: []))) == .unavailable,
              "tunnel stopping during capture rejected")
        check(Net.selectStable(evidence(routeSnapshot: .init(identityComponents: [])), evidence()) == .unavailable,
              "tunnel starting during capture rejected")
        check(Net.select(evidence(primary: nil)) == .unavailable, "missing primary cannot guess sole candidate")
        check(Net.select(evidence(name: "fixture-utun10")) == .unavailable, "virtual primary cannot guess physical default")
        check(Net.select(evidence(primary: "service-other")) == .unavailable, "primary service association required")
        check(Net.select(evidence(indices: [:])) == .unavailable, "known physical type required")
        check(Net.select(evidence(indices: ["fixture-en5": 0])) == .unavailable, "nonzero interface index required")
        check(Net.select(evidence(live: [:])) == .unavailable, "live address required")
        check(Net.select(evidence(live: ["fixture-en5": ["192.168.20.6"]])) == .unavailable, "SC and socket source must agree")
        check(Net.select(evidence(services: ["service-wired": service(active: false)])) == .unavailable, "link must be active")
        check(Net.select(evidence(services: ["service-wired": service(router: "192.168.20.254")])) == .unavailable, "gateway agreement required")
        check(Net.select(evidence(services: ["service-wired": service(interface: "fixture-en0")])) == .unavailable, "service/interface agreement required")
        check(Net.select(evidence(services: ["service-wired": service(addresses: [])])) == .unavailable, "source cannot be absent")
        check(Net.select(evidence(services: ["service-wired": service(masks: [])])) == .unavailable, "source masks required")
        check(Net.select(evidence(services: ["service-wired": service(masks: ["255.0.255.0"])])) == .unavailable, "mask must be contiguous")
        check(Net.select(evidence(services: ["service-wired": service(dhcp: [], dns: [])])) == .unavailable, "no global DNS fallback")
        check(Net.select(evidence(services: ["service-wired": service(dhcp: ["198.18.0.2"], dns: ["192.168.20.1"])])) == .unavailable, "malformed DHCP resolver cannot silently fall back")
        check(Net.select(evidence(services: ["service-wired": service(dhcp: [], dns: ["127.0.0.1"])])) == .unavailable, "loopback TUN service DNS rejected")
        guard case .bound(let staticRoute) = Net.select(evidence(services: ["service-wired": service(dhcp: [], dns: ["192.168.20.1"])]))
        else { fatalError("static physical service DNS should bind") }
        check(staticRoute.dnsServers == ["192.168.20.1"], "static service fallback")
        check(Net.select(evidence(routeSnapshot: .init(identityComponents: ["tun"], physicalDefaults: []))) == .unavailable, "physical default required even sole link")
        check(Net.select(evidence(routeSnapshot: .init(identityComponents: ["tun"], physicalDefaults: [physicalDefault,
            .init(interface: "fixture-en0", gateway: "192.168.1.1")]))) == .unavailable, "multiple defaults cannot choose preference")
        check(Net.select(evidence(routeSnapshot: .init(identityComponents: ["tun"], physicalDefaults: [physicalDefault, physicalDefault]))) != .unavailable,
              "duplicate same default harmless")
        check(Net.select(evidence(primary: nil, name: nil, routeSnapshot: .init(identityComponents: []))) == .system,
              "no active TUN preserves ordinary system transport")
        let ipv6Only = evidence(primary: nil, name: nil, indices: [:], live: [:], services: [:],
            routeSnapshot: .init(identityComponents: []), global: "absent IPv4")
        check(Net.selectStable(ipv6Only, ipv6Only) == .system,
              "coherent IPv6-only network preserves ordinary system probes")
        let ipv6WithTunnel = evidence(primary: nil, name: nil, indices: [:], live: [:], services: [:],
            routeSnapshot: .init(identityComponents: ["tun"]), global: "absent IPv4")
        check(Net.selectStable(ipv6WithTunnel, ipv6WithTunnel) == .unavailable,
              "absent IPv4 under TUN cannot grant bound transport")
        check(Net.selectStable(ipv6Only, ipv6WithTunnel) == .unavailable,
              "TUN starting on IPv6-only network is not coherent system evidence")
        check(Net.select(evidence(global: "different-primary-evidence")) == good, "global metadata is outside the bound physical lease")
        check(Net.select(evidence(services: ["service-wired": service(evidence: "changed-associated-record")])) != good,
              "complete associated service record invalidates route")
        check(Net.select(evidence(services: ["service-wired": service(), "service-wifi": service(interface: "fixture-en0")])) == good,
              "another interface's service cannot invalidate the selected physical lease")
        check(Net.select(evidence(live: ["fixture-en5": ["192.168.20.5", "192.168.20.6"]])) != good,
              "live source address change affects identity")
        check(Net.decodeDNSOption(Data([192, 168, 20, 1, 192, 168, 20, 2])) == ["192.168.20.1", "192.168.20.2"], "DHCP option six decoding")
        for data in [Data(), Data([1, 2, 3]), Data([127, 0, 0, 1]), Data(repeating: 1, count: 36)] {
            check(Net.decodeDNSOption(data) == nil, "malformed DHCP DNS rejected")
        }
        // A known secondary Wi-Fi can be selected through its own scoped physical default,
        // without borrowing the unknown Ethernet primary's descriptor, DNS or source address.
        let wiredDescriptor = NetworkDescriptor(kind: "ethernet", raw: ["gwip": "192.168.20.1", "net": "192.168.20.0/24"])
        let wifiDescriptor = NetworkDescriptor(kind: "wifi", raw: ["gwip": "10.20.0.1", "net": "10.20.0.0/24", "dhcp": "10.20.0.1"])
        let wifiDefault = NetworkContinuitySnapshot.PhysicalDefault(interface: "fixture-en0", gateway: "10.20.0.1")
        let dualRoutes = TunnelRouteSnapshot(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
            boundPhysicalDefaults: [physicalDefault, wifiDefault])
        let dualServices = ["service-wired": service(descriptor: wiredDescriptor),
            "service-wifi": service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.5"],
                dhcp: ["10.20.0.53"], dns: ["198.18.0.2"], evidence: "wifi-record", descriptor: wifiDescriptor)]
        func dual(_ routeSnapshot: TunnelRouteSnapshot = dualRoutes, services: [String: Net.Service] = dualServices,
                  global: String = "primary-record") -> Net.Evidence {
            evidence(indices: ["fixture-en5": 5, "fixture-en0": 4],
                live: ["fixture-en5": ["192.168.20.5"], "fixture-en0": ["10.20.0.5"]],
                services: services, routeSnapshot: routeSnapshot, global: global)
        }
        let candidates = Net.candidates(dual())
        let diagnostics = Net.candidateReportStable(dual(), dual()).diagnostics
        check(diagnostics.physicalInterfaceCount == 2 && diagnostics.serviceCount == 2
              && diagnostics.candidateCount == 2 && diagnostics.rejections.isEmpty,
              "anonymous diagnostics count both stable physical candidates without exporting their fields")

        let teachingIPv4: [String: Any] = ["Router": "10.42.0.1", "Addresses": ["10.42.0.27"],
            "SubnetMasks": ["255.255.255.0"]]
        let teachingService = Net.makeService(interface: "fixture-lab-ethernet", kind: "ethernet",
            ipv4: teachingIPv4, link: ["Active": true], dns: ["ServerAddresses": ["10.42.0.53"]],
            dhcpDNSOption: nil, dhcpServerOption: nil)
        let teachingDefault = NetworkContinuitySnapshot.PhysicalDefault(interface: "fixture-lab-ethernet", gateway: "10.42.0.1")
        let teachingEvidence = Net.Evidence(primaryService: "fixture-lab-service", primaryInterface: "fixture-lab-ethernet",
            indices: ["fixture-lab-ethernet": 8], liveAddresses: ["fixture-lab-ethernet": ["10.42.0.27"]],
            services: ["fixture-lab-service": teachingService], routes: .init(identityComponents: [],
                physicalDefaults: [teachingDefault], boundPhysicalDefaults: [teachingDefault]), globalEvidence: "fixture-lab")
        let teachingReport = Net.candidateReportStable(teachingEvidence, teachingEvidence)
        check(teachingReport.candidates.count == 1 && teachingReport.candidates[0].descriptor?.kind == "ethernet"
              && teachingReport.candidates[0].dnsServers == ["10.42.0.53"],
              "static teaching Ethernet outside dormitory ranges needs neither Wi-Fi identity nor DHCP options")

        func replacingTeachingServices(_ services: [String: Net.Service]) -> Net.Evidence {
            .init(primaryService: teachingEvidence.primaryService, primaryInterface: teachingEvidence.primaryInterface,
                  indices: teachingEvidence.indices, liveAddresses: teachingEvidence.liveAddresses, services: services,
                  routes: teachingEvidence.routes, globalEvidence: teachingEvidence.globalEvidence)
        }
        let malformedOptions: [(Net.CandidateRejection, Data?, Data?)] = [
            (.invalidDHCPDNS, Data([10, 42, 0]), nil),
            (.invalidDHCPServer, nil, Data([10, 42, 0])),
            (.invalidDHCPServer, nil, Data([127, 0, 0, 1]))
        ]
        for (reason, dnsOption, serverOption) in malformedOptions {
            let broken = Net.makeService(interface: "fixture-unrelated", kind: "wifi",
                ipv4: ["Router": "10.43.0.1", "Addresses": ["10.43.0.27"], "SubnetMasks": ["255.255.255.0"]],
                link: ["Active": true], dns: ["ServerAddresses": ["10.43.0.53"]],
                dhcpDNSOption: dnsOption, dhcpServerOption: serverOption)
            var mixedServices = teachingEvidence.services
            mixedServices["fixture-broken"] = broken
            let mixed = replacingTeachingServices(mixedServices)
            let report = Net.candidateReportStable(mixed, mixed)
            check(broken.collectionFailure == reason && report.candidates == teachingReport.candidates
                  && report.diagnostics.rejections[reason.rawValue] == 1,
                  "bad \(reason.rawValue) quarantines only its own NIC and reports a fixed anonymous reason")
            check(Net.select(mixed, interfaceName: "fixture-unrelated") == .unavailable,
                  "a quarantined DHCP record cannot borrow the other interface's route or static resolver")

            var inactiveServices = teachingEvidence.services
            inactiveServices["fixture-inactive"] = Net.makeService(interface: "fixture-unrelated", kind: "wifi",
                ipv4: [:], link: ["Active": false], dns: [:],
                dhcpDNSOption: dnsOption, dhcpServerOption: serverOption)
            let inactiveReport = Net.candidateReport(replacingTeachingServices(inactiveServices))
            check(inactiveReport.candidates == teachingReport.candidates
                  && inactiveReport.diagnostics.rejections == ["inactiveLink": 1],
                  "an inactive interface's stale DHCP cannot obscure the active attachment's diagnostic")

            var duplicateServices = teachingEvidence.services
            duplicateServices["fixture-broken-same-interface"] = Net.makeService(interface: "fixture-lab-ethernet", kind: "ethernet",
                ipv4: teachingIPv4, link: ["Active": true], dns: ["ServerAddresses": ["10.42.0.53"]],
                dhcpDNSOption: dnsOption, dhcpServerOption: serverOption)
            let duplicated = replacingTeachingServices(duplicateServices)
            let duplicateReport = Net.candidateReportStable(duplicated, duplicated)
            check(duplicateReport.candidates.isEmpty && duplicateReport.diagnostics.rejections["duplicateService"] == 2,
                  "a broken same-interface record is retained and cannot turn ambiguous service evidence into authority")
        }

        var noDNSServices = teachingEvidence.services
        noDNSServices["fixture-lab-service"] = Net.makeService(interface: "fixture-lab-ethernet", kind: "ethernet",
            ipv4: teachingIPv4, link: ["Active": true], dns: [:], dhcpDNSOption: nil, dhcpServerOption: nil)
        let missingDNS = replacingTeachingServices(noDNSServices)
        check(Net.candidateReportStable(missingDNS, missingDNS).diagnostics.rejections["missingDNS"] == 1
              && Net.candidates(missingDNS).isEmpty,
              "a static Ethernet record without an associated resolver remains blocked without global DNS fallback")
        let malformed = Net.makeService(interface: "fixture-bad-record", kind: "ethernet", ipv4: teachingIPv4,
            link: ["Active": true], dns: ["fixture-unserializable": NSObject()], dhcpDNSOption: nil, dhcpServerOption: nil)
        check(malformed.collectionFailure == .invalidServiceRecord,
              "an unserializable associated record is rejected locally instead of discarding every physical service")

        let encodedDiagnostics = try JSONEncoder().encode(teachingReport.diagnostics)
        let diagnosticText = String(decoding: encodedDiagnostics, as: UTF8.self)
        let decodedDiagnostics = try JSONDecoder().decode(Net.CandidateDiagnostics.self, from: encodedDiagnostics)
        check(decodedDiagnostics == teachingReport.diagnostics,
              "anonymous candidate diagnostics round trip without serializing physical evidence")
        for privateValue in ["fixture-lab-ethernet", "fixture-lab-service", "10.42", "192.168", "fixture-lab"] {
            check(!diagnosticText.contains(privateValue), "candidate diagnostics never contain names, addresses or service evidence")
        }
        var boundedDiagnostics = Net.CandidateDiagnostics()
        boundedDiagnostics.reject(.missingDNS, count: Int.max)
        boundedDiagnostics.reject(.missingDNS, count: -10)
        check(boundedDiagnostics.rejections == ["missingDNS": 4096], "anonymous reason counters remain nonnegative and bounded")

        let changedTunnelRoutes = TunnelRouteSnapshot(identityComponents: ["fixture-other-tun-route"],
            physicalDefaults: [physicalDefault], boundPhysicalDefaults: [physicalDefault, wifiDefault])
        check(Net.select(dual(changedTunnelRoutes), interfaceName: "fixture-en0")
            == Net.select(dual(), interfaceName: "fixture-en0"),
              "unrelated TUN route changes preserve the selected physical authentication lease")
        check(candidates.count == 2 && candidates.map(\.interfaceName) == ["fixture-en5", "fixture-en0"],
              "verified candidates order primary first independently of interface name")
        check(candidates.first?.descriptor == wiredDescriptor && candidates.first?.serviceID == "service-wired"
            && candidates.first?.isSystemPrimary == true, "primary route keeps its exact associated descriptor")
        check(Net.select(dual()) == Net.select(dual(), interfaceName: "fixture-en5"),
              "implicit primary and explicit capture share one identical transport lease")
        let aliasDescriptor = NetworkContinuitySnapshot.makeDescriptor(kind: "ethernet", ipv4: [
            "Router": "192.168.20.1", "Addresses": ["192.168.30.5", "192.168.20.5", "192.168.30.5"],
            "SubnetMasks": ["255.255.255.0"]])
        let aliasEvidence = evidence(live: ["fixture-en5": ["192.168.20.5", "192.168.30.5"]],
            services: ["service-wired": service(addresses: ["192.168.30.5", "192.168.20.5", "192.168.30.5"],
                descriptor: aliasDescriptor)], routeSnapshot: dualRoutes)
        guard case .bound(let aliasRoute) = Net.select(aliasEvidence, interfaceName: "fixture-en5") else {
            fatalError("live physical aliases must retain the descriptor's selected source")
        }
        check(aliasRoute.addresses == ["192.168.30.5", "192.168.20.5"]
            && aliasRoute.descriptor?.raw["net"] == "192.168.30.0/24",
              "socket source preserves SC first address and the exact descriptor subnet")
        check(Net.select(aliasEvidence) == .bound(aliasRoute)
            && Net.selectStable(aliasEvidence, aliasEvidence, interfaceName: "fixture-en5") == .bound(aliasRoute),
              "implicit and explicitly recaptured multi-address route share identical selected source and lease")
        guard case .bound(let secondary) = Net.select(dual(), interfaceName: "fixture-en0") else {
            fatalError("explicit secondary physical selection must bind")
        }
        check(secondary.serviceID == "service-wifi" && secondary.descriptor == wifiDescriptor && !secondary.isSystemPrimary,
              "secondary keeps service-specific identity without inheriting primary trust")
        check(secondary.addresses == ["10.20.0.5"] && secondary.dnsServers == ["10.20.0.53"],
              "secondary binds own source address and DHCP resolver")
        check(secondary.identity != candidates[0].identity, "selected service has its own physical lease identity")
        let withoutTunnel = TunnelRouteSnapshot(identityComponents: [], physicalDefaults: [physicalDefault],
            boundPhysicalDefaults: [physicalDefault, wifiDefault])
        check(Net.select(dual(withoutTunnel)) == .system, "implicit selection preserves system transport without TUN")
        guard case .bound(let ordinarySecondary) = Net.select(dual(withoutTunnel), interfaceName: "fixture-en0") else {
            fatalError("explicit remembered secondary must bind even without TUN")
        }
        check(ordinarySecondary == secondary,
              "TUN stop retains an unchanged explicitly bound physical lease")
        check(Net.select(dual(), interfaceName: "fixture-missing") == .unavailable, "unavailable requested interface never falls back")
        check(Net.select(dual(.init(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
            boundPhysicalDefaults: [physicalDefault])), interfaceName: "fixture-en0") == .unavailable,
              "secondary requires its own bound default")
        check(Net.select(dual(.init(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
            boundPhysicalDefaults: [physicalDefault, wifiDefault, .init(interface: "fixture-en0", gateway: "10.20.0.254")])),
            interfaceName: "fixture-en0") == .unavailable, "conflicting secondary gateways reject that candidate")
        check(Net.select(dual(.init(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
            boundPhysicalDefaults: [physicalDefault, .init(interface: "fixture-en0", gateway: "10.20.0.254")])),
            interfaceName: "fixture-en0") == .unavailable, "bound default must agree with selected service router")
        var unreliableServices = dualServices
        unreliableServices["service-wifi"]?.descriptor = NetworkDescriptor(kind: "wifi", raw: ["dns": "10.20.0.53"])
        check(Net.candidates(dual(services: unreliableServices)).map(\.interfaceName) == ["fixture-en5"],
              "unreliable descriptor cannot supply remembered-network candidate")
        var ambiguousServices = dualServices
        ambiguousServices["duplicate-wifi"] = dualServices["service-wifi"]
        check(Net.select(dual(services: ambiguousServices), interfaceName: "fixture-en0") == .unavailable,
              "two associated services on the requested interface remain ambiguous")
        check(Net.candidatesStable(dual(), dual()) == candidates, "two consistent physical proofs grant candidate leases")
        check(Net.candidatesStable(dual(), dual(global: "changed")) == candidates,
              "global-only changes between samples preserve consistent physical proofs")
        check(Net.candidatesStable(dual(), dual(withoutTunnel)) == candidates,
              "tunnel changes between samples preserve both independently bound physical proofs")
        check(Net.selectStable(dual(), dual(withoutTunnel), interfaceName: "fixture-en0") == .bound(secondary),
              "explicit selection consistently proves its own route across a TUN change")
        var changedTopology = dualServices
        changedTopology["service-wired"]?.descriptor = NetworkDescriptor(kind: "ethernet", raw: ["gw": "fixture-gateway"])
        check(Net.select(dual(services: changedTopology), interfaceName: "fixture-en0") == .bound(secondary),
              "another physical interface's descriptor cannot revoke the selected secondary lease")
        let switchedPrimary = Net.Evidence(primaryService: "service-wifi", primaryInterface: "fixture-en0",
            indices: ["fixture-en5": 5, "fixture-en0": 4],
            liveAddresses: ["fixture-en5": ["192.168.20.5"], "fixture-en0": ["10.20.0.5"]],
            services: dualServices, routes: .init(identityComponents: ["fixture-tun-route"], physicalDefaults: [wifiDefault],
                boundPhysicalDefaults: [physicalDefault, wifiDefault]), globalEvidence: "wifi-primary")
        guard case .bound(let newlyPrimarySecondary) = Net.select(switchedPrimary, interfaceName: "fixture-en0") else {
            fatalError("same selected interface remains bound after system primary changes")
        }
        check(newlyPrimarySecondary.isSystemPrimary && !secondary.isSystemPrimary && newlyPrimarySecondary == secondary,
              "system primary is refreshed selection metadata, not physical transport authority")
        check(Net.selectStable(dual(), switchedPrimary, interfaceName: "fixture-en0") == .bound(secondary),
              "a system primary switch cannot break two consistent proofs of an explicitly selected interface")
        let stableAcrossPrimary = Net.candidatesStable(dual(), switchedPrimary)
        check(stableAcrossPrimary.count == 2 && stableAcrossPrimary.first?.interfaceName == "fixture-en0"
            && stableAcrossPrimary.first?.isSystemPrimary == true,
              "stable candidates expose current preference while retaining both physical leases")
        check(Net.selectStable(dual(), switchedPrimary) == .unavailable,
              "implicit system-primary selection cannot mix different primary interfaces across samples")
        let wifiOnly = Net.Evidence(primaryService: "service-wifi", primaryInterface: "fixture-en0",
            indices: ["fixture-en0": 4], liveAddresses: ["fixture-en0": ["10.20.0.5"]],
            services: ["service-wifi": dualServices["service-wifi"]!], routes: .init(identityComponents: [],
                physicalDefaults: [wifiDefault], boundPhysicalDefaults: [wifiDefault]), globalEvidence: "wired-removed")
        check(Net.selectStable(dual(), wifiOnly, interfaceName: "fixture-en0") == .bound(secondary),
              "removing another NIC and stopping TUN cannot invalidate a coherent selected Wi-Fi route")
        check(Net.candidatesStable(wifiOnly, dual()).map(\.interfaceName) == ["fixture-en0"],
              "a newly appearing interface receives no candidate authority until both samples prove it")
        let unstableReport = Net.candidateReportStable(wifiOnly, dual())
        check(unstableReport.diagnostics.candidateCount == 1 && unstableReport.diagnostics.rejections["unstableSnapshot"] == 1,
              "a changing physical candidate reports instability without granting it authority")
        let noDefaultReport = Net.candidateReport(dual(.init(identityComponents: [], physicalDefaults: [], boundPhysicalDefaults: [])))
        check(noDefaultReport.candidates.isEmpty && noDefaultReport.diagnostics.rejections == ["missingDefaultRoute": 2],
              "missing bound defaults have a distinct anonymous reason for both physical interfaces")
        let absentReport = Net.candidateReport(ipv6Only)
        check(absentReport.diagnostics.candidateCount == 0 && absentReport.diagnostics.rejections == ["noIPv4Service": 1],
              "a coherent network without IPv4 service is distinguished from native collection failure")
        for (label, changed) in [
            ("interface index", Net.Evidence(primaryService: "service-wired", primaryInterface: "fixture-en5",
                indices: ["fixture-en5": 5, "fixture-en0": 40], liveAddresses: dual().liveAddresses,
                services: dualServices, routes: dualRoutes, globalEvidence: "primary-record")),
            ("live source", Net.Evidence(primaryService: "service-wired", primaryInterface: "fixture-en5",
                indices: dual().indices, liveAddresses: ["fixture-en5": ["192.168.20.5"], "fixture-en0": ["10.20.0.6"]],
                services: dualServices, routes: dualRoutes, globalEvidence: "primary-record")),
            ("bound default", dual(.init(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
                boundPhysicalDefaults: [physicalDefault, .init(interface: "fixture-en0", gateway: "10.20.0.254")]))),
            ("missing bound default", dual(.init(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
                boundPhysicalDefaults: [physicalDefault])))
        ] {
            check(Net.selectStable(dual(), changed, interfaceName: "fixture-en0") == .unavailable,
                  "selected \(label) changes fail the two-sample proof")
            check(Net.candidatesStable(dual(), changed).map(\.interfaceName) == ["fixture-en5"],
                  "selected \(label) changes do not revoke the independent wired proof")
        }
        for (label, changedService) in [
            ("source address", service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.6"],
                dhcp: ["10.20.0.53"], evidence: "wifi-record", descriptor: wifiDescriptor)),
            ("subnet mask", service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.5"], masks: ["255.255.0.0"],
                dhcp: ["10.20.0.53"], evidence: "wifi-record", descriptor: wifiDescriptor)),
            ("DNS", service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.5"],
                dhcp: ["10.20.0.54"], evidence: "wifi-record", descriptor: wifiDescriptor)),
            ("descriptor", service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.5"],
                dhcp: ["10.20.0.53"], evidence: "wifi-record", descriptor: .init(kind: "wifi", raw: ["gwip": "10.20.0.1", "net": "10.20.0.0/24", "gw": "fixture-new-gateway"]))),
            ("associated record", service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.5"],
                dhcp: ["10.20.0.53"], evidence: "changed-wifi-record", descriptor: wifiDescriptor)),
            ("link", service(interface: "fixture-en0", router: "10.20.0.1", addresses: ["10.20.0.5"], active: false,
                dhcp: ["10.20.0.53"], evidence: "wifi-record", descriptor: wifiDescriptor))
        ] {
            var changedServices = dualServices
            changedServices["service-wifi"] = changedService
            check(Net.selectStable(dual(), dual(services: changedServices), interfaceName: "fixture-en0") == .unavailable,
                  "selected \(label) changes remain invalidating even when display topology is unchanged")
            check(Net.candidatesStable(dual(), dual(services: changedServices)).map(\.interfaceName) == ["fixture-en5"],
                  "selected \(label) changes preserve only the independent unchanged wired proof")
        }
        var movedServices = dualServices
        movedServices["service-wifi"] = service(interface: "fixture-en0", router: "10.21.0.1", addresses: ["10.21.0.5"],
            dhcp: ["10.21.0.53"], evidence: "wifi-moved-record",
            descriptor: .init(kind: "wifi", raw: ["gwip": "10.21.0.1", "net": "10.21.0.0/24", "dhcp": "10.21.0.1"]))
        let movedWifi = Net.Evidence(primaryService: "service-wired", primaryInterface: "fixture-en5", indices: dual().indices,
            liveAddresses: ["fixture-en5": ["192.168.20.5"], "fixture-en0": ["10.21.0.5"]], services: movedServices,
            routes: .init(identityComponents: ["fixture-tun-route"], physicalDefaults: [physicalDefault],
                boundPhysicalDefaults: [physicalDefault, .init(interface: "fixture-en0", gateway: "10.21.0.1")]),
            globalEvidence: "primary-record")
        guard case .bound(let movedRoute) = Net.select(movedWifi, interfaceName: "fixture-en0") else {
            fatalError("a coherent new attachment must acquire a different physical proof")
        }
        check(movedRoute != secondary && movedRoute.identity != secondary.identity,
              "a coherently changed source, gateway, resolver and descriptor cannot inherit the old lease")
        check(Net.selectStable(dual(), movedWifi, interfaceName: "fixture-en0") == .unavailable,
              "two individually valid but different attachments cannot authorize the selected interface")
        check(Net.selectStable(movedWifi, movedWifi, interfaceName: "fixture-en0") == .bound(movedRoute),
              "a new attachment receives authority only after its own two consistent samples")
        // Cancellation wins before any interface lookup/socket operation. Fixtures never send packets.
        do { _ = try DNS.resolveA(host: "net.szu.edu.cn", route: bound, cancelled: { true }); check(false, "early cancellation") }
        catch { check(error as? CampusPhysicalDNSError == .cancelled, "early cancellation") }
        for timeout in [0.0, -1, Double.infinity, Double.nan, 31] {
            do { _ = try DNS.resolveA(host: "net.szu.edu.cn", route: bound, timeout: timeout); check(false, "timeout bounds") }
            catch { check(error as? CampusPhysicalDNSError == .unavailable, "timeout bounds") }
        }
        do { _ = try DNS.resolveA(host: "evil.example", route: bound); check(false, "resolver host allowlist") }
        catch { check(error as? CampusPhysicalDNSError == .unavailable, "resolver host allowlist") }
        do { _ = try DNS.resolveA(host: "net.szu.edu.cn", route: bound); check(false, "stale interface index") }
        catch { check(error as? CampusPhysicalDNSError == .unavailable, "stale interface index") }
        print("CampusPhysicalNetwork: \(checks) assertions passed")
    }
}
