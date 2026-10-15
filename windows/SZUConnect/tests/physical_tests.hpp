#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// Injected network evidence and DNS packets only. No sockets or real network data.
inline physical::Attachment physical_attachment(std::string key = "offline-fixture-physical") {
    physical::Attachment a;
    a.physical = a.up = true;
    a.route.interface_key = std::move(key);
    a.route.interface_index = 7; a.route.interface_luid = 700;
    a.route.source_ipv4 = "192.0.2.17"; a.route.addresses = {"192.0.2.17", "192.0.2.23"};
    a.route.dns_servers = {"192.0.2.53"}; a.route.gateway_ipv4 = "192.0.2.1";
    a.route.descriptor.kind = "ethernet";
    a.route.descriptor.raw = {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}, {"dns", "192.0.2.53"}, {"dhcp", "192.0.2.2"}};
    a.bound_default_gateways = {"192.0.2.1"};
    a.route.display_name = "Offline fixture"; a.route.is_system_primary = true;
    return a;
}
inline physical::Packet physical_answer(std::string_view host = "net.szu.edu.cn", std::uint16_t tx = 0x1234) {
    auto p = physical::dns_query(host, tx);
    p[2] = 0x81; p[3] = 0x80; p[7] = 1;
    p.insert(p.end(), {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 192, 0, 2, 80});
    return p;
}
inline physical::Packet physical_name(std::string_view host) {
    physical::Packet p;
    std::size_t start = 0;
    while (start < host.size()) {
        const auto at = host.find('.', start), end = at == std::string_view::npos ? host.size() : at;
        p.push_back(static_cast<std::uint8_t>(end - start));
        for (std::size_t i = start; i < end; ++i) p.push_back(static_cast<std::uint8_t>(host[i]));
        start = end + 1;
    }
    p.push_back(0); return p;
}
inline bool physical_rejects(const physical::Packet& packet) {
    try { (void)physical::dns_parse(packet, "net.szu.edu.cn", 0x1234); }
    catch (const physical::Error& e) { return e.reason == physical::Failure::invalidResponse; }
    return false;
}
inline void physical_tests(Harness& t) {
    t.begin("physical-requires-positive-hardware");
    physical::HardwareEvidence hardware;
    hardware.expected_luid = hardware.actual_luid = 700;
    hardware.expected_index = hardware.actual_index = 7;
    hardware.readable = hardware.up = true;
    t.check(!physical::physical_hardware_confirmed(hardware), "unknown virtual type-6 miniport cannot become physical via its name or IfType");
    hardware.hardware_interface = true;
    t.check(physical::physical_hardware_confirmed(hardware), "matching UP real hardware can proceed to source gateway DNS proof");
    const auto confirmed = hardware;
    hardware.readable = false;
    t.check(!physical::physical_hardware_confirmed(hardware), "unreadable hardware status fails closed");
    hardware = confirmed; hardware.actual_luid = 701;
    t.check(!physical::physical_hardware_confirmed(hardware), "different interface LUID rejects mixed snapshots");
    hardware = confirmed; hardware.actual_index = 8;
    t.check(!physical::physical_hardware_confirmed(hardware), "different interface index rejects mixed snapshots");
    hardware = confirmed; hardware.up = false;
    t.check(!physical::physical_hardware_confirmed(hardware), "hardware must still be UP in GetIfEntry2");
    hardware = confirmed; hardware.filter_interface = true;
    t.check(!physical::physical_hardware_confirmed(hardware), "hardware filter cannot become authentication candidate");
    hardware = confirmed; hardware.endpoint_interface = true;
    t.check(!physical::physical_hardware_confirmed(hardware), "endpoint without external connectivity cannot become candidate");
    hardware = confirmed; hardware.actual_luid = hardware.expected_luid = 0;
    t.check(!physical::physical_hardware_confirmed(hardware), "zero LUID is not positive hardware evidence");
    hardware = confirmed; hardware.actual_index = hardware.expected_index = 0;
    t.check(!physical::physical_hardware_confirmed(hardware), "zero index is not positive hardware evidence");

    t.begin("physical-preserve-existing-other-link");
    physical::OtherLinkEvidence other_link;
    t.check(!physical::other_physical_link_active(other_link), "absent or disabled adapter allows Wi-Fi finding");
    other_link.up = true; other_link.usable_source = true;
    t.check(physical::other_physical_link_active(other_link), "UP unknown attachment with usable source is preserved without a gateway");
    other_link.details_readable = true; other_link.hardware_interface = true; other_link.media_connected = true;
    t.check(physical::other_physical_link_active(other_link), "connected WWAN hardware with source blocks Wi-Fi takeover");
    other_link.usable_source = false;
    t.check(physical::other_physical_link_active(other_link), "connected real hardware still acquiring IP must not be taken over");
    other_link.hardware_interface = false; other_link.connector_present = true;
    t.check(physical::other_physical_link_active(other_link), "confirmed physical connector and connected media protects other hardware");
    other_link.connector_present = false;
    t.check(!physical::other_physical_link_active(other_link), "software connected flag without usable source does not block Wi-Fi forever");
    other_link.usable_source = true;
    t.check(physical::other_physical_link_active(other_link), "unclassified UP usable attachment conservatively blocks takeover");
    other_link.known_virtual = true; other_link.hardware_interface = true;
    t.check(!physical::other_physical_link_active(other_link), "known TUN Hyper-V loopback or endpoint is excluded even with source and connected flags");
    other_link.hardware_interface = false; other_link.usable_source = false;
    t.check(!physical::other_physical_link_active(other_link), "idle virtual tunnel does not block Wi-Fi finding");
    other_link.known_virtual = false; other_link.hardware_interface = true; other_link.usable_source = true;
    other_link.media_connected = false; other_link.media_disconnected = true;
    t.check(!physical::other_physical_link_active(other_link), "confirmed disconnected hardware ignores stale configured IP");
    other_link.details_readable = false;
    t.check(physical::other_physical_link_active(other_link), "failed interface-details read cannot assert stale disconnected flag");
    other_link.up = false;
    t.check(!physical::other_physical_link_active(other_link), "DOWN hardware cannot claim active connection");
    other_link.up = true; other_link.usable_source = false; other_link.media_disconnected = false; other_link.media_connected = false;
    t.check(!physical::other_physical_link_active(other_link), "unknown UP adapter with only link-local source and no connected hardware is idle");

    t.begin("physical-attachment");
    const auto a = physical_attachment();
    const auto routes = physical::candidates({a});
    t.equal(routes.size(), std::size_t(1), "one independently proven physical candidate");
    if (routes.size() != 1) return;
    t.equal(routes[0].source_ipv4, a.route.source_ipv4, "source belongs to descriptor subnet");
    t.equal(routes[0].identity.size(), std::size_t(64), "RAM identity hashes selected attachment");
    auto metadata = a;
    metadata.route.display_name = "Renamed fixture"; metadata.route.is_system_primary = false;
    t.equal(physical::stable_candidates({a}, {metadata}).size(), std::size_t(1), "name and primary preference never revoke a bound source");
    t.check(physical::candidates({metadata})[0] == routes[0], "route equality ignores UI metadata");
    auto other = physical_attachment("offline-fixture-other"); other.route.interface_index = 8; other.route.interface_luid = 800;
    auto tunnel = physical_attachment("offline-fixture-tunnel"); tunnel.physical = false;
    t.equal(physical::stable_candidates({a}, {a, other, tunnel}).size(), std::size_t(1), "new NIC and TUN do not cancel selected attachment");
    auto changed_other = other; changed_other.route.dns_servers = {"192.0.2.54"}; changed_other.route.descriptor.raw["dns"] = "192.0.2.54";
    const auto stable = physical::stable_candidates({a, other, tunnel}, {metadata, changed_other});
    t.equal(stable.size(), std::size_t(1), "other NIC DNS and removed TUN only invalidate other candidate");
    t.check(stable[0] == routes[0], "selected identity independent of other topology");
    const auto invalidates = [&](physical::Attachment changed, const std::string& label) {
        t.check(physical::stable_candidates({a}, {changed}).empty(), label);
    };
    auto changed = a; changed.route.source_ipv4 = "192.0.2.23"; changed.route.addresses = {"192.0.2.23", "192.0.2.17"};
    invalidates(changed, "actual source change invalidates lease");
    changed = a; changed.route.interface_index = 9; invalidates(changed, "interface index reuse invalidates lease");
    changed = a; changed.route.interface_luid = 900; invalidates(changed, "different LUID invalidates lease");
    changed = a; changed.route.addresses.push_back("192.0.2.24"); invalidates(changed, "selected interface address change invalidates lease");
    changed = a; changed.route.gateway_ipv4 = "192.0.2.3"; changed.route.descriptor.raw["gwip"] = "192.0.2.3";
    changed.bound_default_gateways = {"192.0.2.3"}; invalidates(changed, "selected default gateway change invalidates lease");
    changed = a; changed.route.dns_servers = {"192.0.2.54"}; changed.route.descriptor.raw["dns"] = "192.0.2.54";
    invalidates(changed, "associated DNS change invalidates lease");
    changed = a; changed.route.descriptor.raw["dhcp"] = "192.0.2.3"; invalidates(changed, "selected DHCP identity change invalidates lease");
    changed = a; changed.up = false; invalidates(changed, "link down invalidates lease");
    changed = a; changed.bound_default_gateways.push_back("192.0.2.3"); invalidates(changed, "two distinct bound defaults fail closed");
    changed = a; changed.bound_default_gateways.clear(); invalidates(changed, "missing interface-specific default fails closed");
    changed = a; changed.bound_default_gateways.push_back("192.0.2.1");
    t.equal(physical::stable_candidates({a}, {changed}).size(), std::size_t(1), "duplicate identical route rows do not flap lease");
    changed = a; changed.route.source_ipv4 = "192.0.3.17"; changed.route.addresses[0] = changed.route.source_ipv4;
    invalidates(changed, "source outside descriptor subnet cannot authorize");
    changed = a; changed.route.dns_servers = {"198.18.0.1"}; changed.route.descriptor.raw["dns"] = "198.18.0.1";
    invalidates(changed, "fake DNS resolver cannot become physical authority");
    changed = a; changed.route.descriptor.raw["dns"] = "192.0.2.54"; invalidates(changed, "descriptor and socket resolver must agree");
    t.check(physical::candidates({a, a}).empty(), "duplicate evidence for same interface is ambiguous");
    for (const auto ip : {"0.1.2.3", "127.0.0.1", "169.254.1.1", "198.18.0.1", "198.19.2.3", "224.0.0.1", "255.255.255.255",
                          "1.2.3", "01.2.3.4", "1.2.3.256", "1.2.3.4.5", "::1", "1.2.3.4x"})
        t.check(!physical::usable_ipv4(ip), std::string("reject unusable IPv4 ") + ip);
    for (const auto ip : {"192.0.2.17", "10.0.0.1", "172.30.255.42", "198.20.0.1"})
        t.check(physical::usable_ipv4(ip), std::string("allow canonical unicast ") + ip);
    t.check(physical::source_in_network("192.0.2.17", "192.0.2.0/24"), "source subnet match");
    for (const auto net : {"192.0.2.1/24", "192.0.3.0/24", "192.0.2.0/0", "192.0.2.0/33", "192.0.2.0/", "192.0.2.0/abc"})
        t.check(!physical::source_in_network("192.0.2.17", net), std::string("reject invalid subnet ") + net);

    t.begin("physical-dns-packet");
    const auto good = physical_answer();
    t.equal(physical::dns_parse(good, "net.szu.edu.cn", 0x1234), std::vector<std::string>{"192.0.2.80"}, "matching A response");
    for (const auto host : {"net.szu.edu.cn", "www.apple.com", "www.msftconnecttest.com"})
        t.equal(physical::dns_parse(physical_answer(host), host, 0x1234).size(), std::size_t(1), "all three fixed hosts supported");
    for (const auto host : {"example.test", "NET.SZU.EDU.CN", "net.szu.edu.cn.", "net.szu.edu.cn.evil.test", "172.30.255.42"}) {
        bool rejected = false;
        try { (void)physical::dns_query(host, 1); } catch (const physical::Error& e) { rejected = e.reason == physical::Failure::unavailable; }
        t.check(rejected, std::string("fixed-host query rejects ") + host);
    }
    auto bad = good; bad[0] ^= 1; t.check(physical_rejects(bad), "wrong transaction rejected");
    bad = good; bad[2] = 0x01; t.check(physical_rejects(bad), "query is not answer");
    bad = good; bad[2] |= 0x08; t.check(physical_rejects(bad), "nonstandard opcode rejected");
    bad = good; bad[2] |= 2; t.check(physical_rejects(bad), "truncated UDP never supplies address");
    bad = good; bad[3] |= 3; t.check(physical_rejects(bad), "RCODE failure rejected");
    bad = good; bad[3] |= 0x40; t.check(physical_rejects(bad), "reserved flag rejected");
    bad = good; bad[5] = 2; t.check(physical_rejects(bad), "multiple questions rejected");
    bad = good; bad[13] = 'x'; t.check(physical_rejects(bad), "different question rejected");
    bad = good; bad[good.size() - 4] = 198; bad[good.size() - 3] = 18; t.check(physical_rejects(bad), "fake A address rejected");
    bad = good; bad.back() = 0; bad[good.size() - 4] = 127; t.check(physical_rejects(bad), "loopback A rejected");
    bad = good; bad.push_back(0); t.check(physical_rejects(bad), "trailing bytes rejected");
    bad = good; bad.resize(bad.size() - 1); t.check(physical_rejects(bad), "incomplete RDATA rejected");
    bad = good; bad[7] = 2; t.check(physical_rejects(bad), "missing second answer rejected");
    const auto question_end = physical::dns_query("net.szu.edu.cn", 0x1234).size();
    bad = good; bad[question_end + 1] = static_cast<std::uint8_t>(question_end); t.check(physical_rejects(bad), "self compression pointer rejected");
    bad = good; bad[question_end + 1] = 0; t.check(physical_rejects(bad), "compression into header rejected");
    bad = good; bad[question_end + 1] = static_cast<std::uint8_t>(question_end + 2); t.check(physical_rejects(bad), "forward compression rejected");
    for (std::size_t n = 0; n < question_end + 10; ++n) {
        auto cut = good; cut.resize(n);
        t.check(physical_rejects(cut), "every truncated header/question/answer fails closed");
    }
    auto cname = physical::dns_query("net.szu.edu.cn", 0x1234); cname[2] = 0x81; cname[3] = 0x80; cname[7] = 2;
    const auto alias = physical_name("campus.offline.test");
    cname.insert(cname.end(), {0xc0, 0x0c, 0, 5, 0, 1, 0, 0, 0, 10, 0, static_cast<std::uint8_t>(alias.size())});
    cname.insert(cname.end(), alias.begin(), alias.end());
    cname.insert(cname.end(), alias.begin(), alias.end());
    cname.insert(cname.end(), {0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 192, 0, 2, 81});
    t.equal(physical::dns_parse(cname, "net.szu.edu.cn", 0x1234), std::vector<std::string>{"192.0.2.81"}, "same-packet CNAME ownership chain");
    auto unrelated = good; unrelated.erase(unrelated.begin() + static_cast<std::ptrdiff_t>(question_end), unrelated.begin() + static_cast<std::ptrdiff_t>(question_end + 2));
    unrelated.insert(unrelated.begin() + static_cast<std::ptrdiff_t>(question_end), alias.begin(), alias.end());
    t.check(physical_rejects(unrelated), "unrelated answer owner cannot supply A address");
    auto no_chain = cname; no_chain.resize(no_chain.size() - alias.size() - 14); no_chain[7] = 1;
    t.check(physical_rejects(no_chain), "CNAME without same-packet A cannot fall back to global DNS");
    auto conflict = good; conflict[7] = 2;
    conflict.insert(conflict.end(), {0xc0, 0x0c, 0, 5, 0, 1, 0, 0, 0, 10, 0, static_cast<std::uint8_t>(alias.size())});
    conflict.insert(conflict.end(), alias.begin(), alias.end());
    t.check(physical_rejects(conflict), "owner cannot supply both CNAME and A");
    auto cycle = physical::dns_query("net.szu.edu.cn", 0x1234); cycle[2] = 0x81; cycle[3] = 0x80; cycle[7] = 1;
    cycle.insert(cycle.end(), {0xc0, 0x0c, 0, 5, 0, 1, 0, 0, 0, 10, 0, 2, 0xc0, 0x0c});
    t.check(physical_rejects(cycle), "CNAME cycle fails without an external lookup");
    auto additional = good; additional[7] = 0; additional[11] = 1;
    t.check(physical_rejects(additional), "additional-section A alone cannot authorize queried host");
    bad = good; bad[question_end + 3] = 28;
    t.check(physical_rejects(bad), "IPv6 answer alone cannot supply physical IPv4");

    t.begin("physical-dns-exchange");
    const auto simulate = [&](int scenario, std::size_t servers, double deadline, physical::Failure expected, bool success) {
        double clock = 0; bool stopped = false, delivered = false;
        std::vector<std::pair<std::size_t, double>> sent;
        physical::ExchangeHooks h;
        h.now = [&] { return clock; }; h.cancelled = [&] { return stopped; };
        h.send = [&](std::size_t server, const physical::Packet& packet) {
            t.equal(packet, physical::dns_query("net.szu.edu.cn", 0x1234), "retransmission retains exact question and transaction");
            sent.push_back({server, clock});
            if (scenario == 6 || scenario == 12) stopped = true;
            return scenario != 7 && scenario != 12;
        };
        h.receive = [&](const std::vector<std::size_t>& active, double wait) {
            clock += wait;
            std::vector<physical::ExchangeEvent> events;
            if (scenario == 1 && sent.size() >= 2 && !delivered) { delivered = true; events.push_back({physical::ExchangeEvent::Kind::packet, 0, good}); }
            if (scenario == 2) { stopped = true; events.push_back({physical::ExchangeEvent::Kind::packet, 0, good}); }
            if (scenario == 3 && !delivered) { delivered = true; auto p = good; p[3] |= 3; events.push_back({physical::ExchangeEvent::Kind::packet, 0, p}); }
            if (scenario == 4 && !delivered) { delivered = true; auto p = good; p[2] |= 2; events.push_back({physical::ExchangeEvent::Kind::packet, 0, p}); }
            if (scenario == 5 && !delivered) { delivered = true; auto p = good; p[0] ^= 1; events.push_back({physical::ExchangeEvent::Kind::packet, 0, p}); }
            if (scenario == 8) { clock = deadline; events.push_back({physical::ExchangeEvent::Kind::packet, 0, good}); }
            if (scenario == 9 && !delivered) {
                delivered = true; auto p = good; p[3] |= 3;
                events.push_back({physical::ExchangeEvent::Kind::packet, 0, p});
                events.push_back({physical::ExchangeEvent::Kind::packet, 1, good});
            }
            if (scenario == 10 && !active.empty()) events.push_back({physical::ExchangeEvent::Kind::failed, active.front(), {}});
            return events;
        };
        if (scenario == 11) stopped = true;
        bool answered = false; std::optional<physical::Failure> failure;
        try { answered = physical::dns_exchange("net.szu.edu.cn", 0x1234, servers, deadline, h) == std::vector<std::string>{"192.0.2.80"}; }
        catch (const physical::Error& e) { failure = e.reason; }
        t.equal(answered, success, "exchange success matches injected fault");
        if (!success) t.check(failure == expected, "exchange failure preserves cancellation/budget/terminal semantics");
        t.check(clock <= deadline + 0.000001, "all resolver attempts share one deadline");
        if (scenario == 0) {
            t.equal(sent.size(), std::size_t(3), "bounded initial + 250ms + 750ms sends");
            if (sent.size() == 3) { t.check(std::abs(sent[1].second - 0.25) < 0.000001, "first loss retry at 250ms"); t.check(std::abs(sent[2].second - 0.75) < 0.000001, "second loss retry at 750ms"); }
        }
        if (scenario == 1) t.check(clock < 0.4, "first lost packet recovers on short retry");
        if (scenario == 3 || scenario == 4) t.check(clock < 0.1 && sent.size() == 1, "valid RCODE/TC ends that resolver promptly");
        if (scenario == 5) t.equal(sent.size(), std::size_t(3), "stray wrong transaction cannot stop finite retries");
        if (scenario == 11) t.check(sent.empty(), "already cancelled never sends DNS");
    };
    simulate(0, 1, 1, physical::Failure::timeout, false);
    simulate(1, 1, 1, physical::Failure::timeout, true);
    simulate(2, 1, 1, physical::Failure::cancelled, false);
    simulate(3, 1, 1, physical::Failure::invalidResponse, false);
    simulate(4, 1, 1, physical::Failure::invalidResponse, false);
    simulate(5, 1, 1, physical::Failure::invalidResponse, false);
    simulate(6, 1, 1, physical::Failure::cancelled, false);
    simulate(7, 1, 1, physical::Failure::unavailable, false);
    simulate(8, 1, 1, physical::Failure::timeout, false);
    simulate(9, 2, 1, physical::Failure::timeout, true);
    simulate(10, 1, 1, physical::Failure::unavailable, false);
    simulate(11, 1, 1, physical::Failure::cancelled, false);
    simulate(12, 1, 1, physical::Failure::cancelled, false);
}
