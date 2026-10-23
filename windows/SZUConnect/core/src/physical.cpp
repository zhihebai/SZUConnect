#include "szu/physical.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace szu::physical {
namespace {
std::optional<std::uint32_t> ipv4(std::string_view value) {
    std::uint32_t out = 0;
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const auto stop = value.find('.', start);
        const auto end = stop == std::string_view::npos ? value.size() : stop;
        if (end == start || end - start > 3 || (end - start > 1 && value[start] == '0')) return {};
        unsigned part = 0;
        for (std::size_t n = start; n < end; ++n) {
            if (value[n] < '0' || value[n] > '9') return {};
            part = part * 10 + static_cast<unsigned>(value[n] - '0');
        }
        if (part > 255 || (i == 3) != (stop == std::string_view::npos)) return {};
        out = (out << 8) | part;
        start = end + 1;
    }
    return out;
}
void canonical(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}
void field(std::string& material, std::string_view value) {
    material += std::to_string(value.size()); material += ':'; material += value;
}
std::uint16_t u16(const Packet& p, std::size_t n) {
    if (n + 2 > p.size()) throw Error(Failure::invalidResponse);
    return static_cast<std::uint16_t>((std::uint16_t(p[n]) << 8) | p[n + 1]);
}
std::string name(const Packet& p, std::size_t& offset) {
    std::size_t cursor = offset, next = std::string::npos, expanded = 0;
    std::set<std::size_t> seen;
    std::string out;
    for (int hop = 0; hop < 128; ++hop) {
        if (cursor >= p.size() || !seen.insert(cursor).second) throw Error(Failure::invalidResponse);
        const auto length = p[cursor];
        if ((length & 0xc0) == 0xc0) {
            const auto pointer = static_cast<std::size_t>(u16(p, cursor) & 0x3fff);
            if (pointer < 12 || pointer >= cursor) throw Error(Failure::invalidResponse);
            if (next == std::string::npos) next = cursor + 2;
            cursor = pointer;
            continue;
        }
        if ((length & 0xc0) || length > 63) throw Error(Failure::invalidResponse);
        ++cursor;
        if (!length) { offset = next == std::string::npos ? cursor : next; return out; }
        if (cursor + length > p.size() || expanded + length + 1 > 255) throw Error(Failure::invalidResponse);
        if (!out.empty()) out += '.';
        for (std::size_t i = 0; i < length; ++i) {
            char c = static_cast<char>(p[cursor + i]);
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-'))
                throw Error(Failure::invalidResponse);
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            out += c;
        }
        expanded += length + 1; cursor += length;
    }
    throw Error(Failure::invalidResponse);
}
bool exact_question(const Packet& p, std::string_view host, std::uint16_t tx, std::size_t& offset) {
    if (p.size() < 12 || p.size() > 65535 || u16(p, 0) != tx || (u16(p, 2) & 0xf800) != 0x8000 ||
        (u16(p, 2) & 0x0040) || u16(p, 4) != 1) return false;
    offset = 12;
    if (name(p, offset) != host || offset + 4 > p.size() || u16(p, offset) != 1 || u16(p, offset + 2) != 1) return false;
    offset += 4;
    return true;
}
enum class Reply { ignored, terminal, answer };
Reply classify(const Packet& p, std::string_view host, std::uint16_t tx, std::vector<std::string>& answer) {
    try {
        std::size_t offset = 0;
        if (!exact_question(p, host, tx, offset)) return Reply::ignored;
        if ((u16(p, 2) & 0x020f) || (u16(p, 6) == 0 && u16(p, 8) == 0 && u16(p, 10) == 0 && offset == p.size()))
            return Reply::terminal;
        answer = dns_parse(p, host, tx);
        return Reply::answer;
    } catch (const Error&) { return Reply::ignored; }
}
}  // namespace

bool Route::operator==(const Route& o) const {
    return interface_key == o.interface_key && interface_index == o.interface_index && interface_luid == o.interface_luid &&
        source_ipv4 == o.source_ipv4 && addresses == o.addresses && dns_servers == o.dns_servers &&
        gateway_ipv4 == o.gateway_ipv4 && descriptor.kind == o.descriptor.kind && descriptor.raw == o.descriptor.raw && identity == o.identity;
}

bool usable_ipv4(std::string_view value) {
    const auto parsed = ipv4(value);
    if (!parsed) return false;
    const auto a = *parsed >> 24, b = (*parsed >> 16) & 255;
    return a != 0 && a != 127 && a < 224 && !(a == 169 && b == 254) && !(a == 198 && (b == 18 || b == 19));
}

bool source_in_network(std::string_view source, std::string_view network) {
    const auto slash = network.find('/');
    if (slash == std::string_view::npos || slash + 1 == network.size() || network.size() - slash - 1 > 2) return false;
    const auto ip = ipv4(source), net = ipv4(network.substr(0, slash));
    unsigned prefix = 0;
    for (const char c : network.substr(slash + 1)) {
        if (c < '0' || c > '9') return false;
        prefix = prefix * 10 + static_cast<unsigned>(c - '0');
    }
    if (!ip || !net || prefix == 0 || prefix > 32) return false;
    const std::uint32_t mask = prefix == 32 ? 0xffffffffu : (0xffffffffu << (32 - prefix));
    return (*net & mask) == *net && (*ip & mask) == *net;
}

std::string_view to_string(CandidateRejection reason) {
    switch (reason) {
        case CandidateRejection::systemSnapshotUnavailable: return "systemSnapshotUnavailable";
        case CandidateRejection::duplicateInterface: return "duplicateInterface";
        case CandidateRejection::unsupportedInterface: return "unsupportedInterface";
        case CandidateRejection::inactiveLink: return "inactiveLink";
        case CandidateRejection::missingInterfaceIndex: return "missingInterfaceIndex";
        case CandidateRejection::missingSourceAddress: return "missingSourceAddress";
        case CandidateRejection::invalidSourceAddress: return "invalidSourceAddress";
        case CandidateRejection::invalidSubnet: return "invalidSubnet";
        case CandidateRejection::invalidGateway: return "invalidGateway";
        case CandidateRejection::missingDNS: return "missingDNS";
        case CandidateRejection::invalidDNS: return "invalidDNS";
        case CandidateRejection::missingDescriptor: return "missingDescriptor";
        case CandidateRejection::missingDefaultRoute: return "missingDefaultRoute";
        case CandidateRejection::conflictingDefaultRoutes: return "conflictingDefaultRoutes";
        case CandidateRejection::gatewayMismatch: return "gatewayMismatch";
        case CandidateRejection::associatedDNSMismatch: return "associatedDNSMismatch";
        case CandidateRejection::invalidDHCPServer: return "invalidDHCPServer";
        case CandidateRejection::unstableSnapshot: return "unstableSnapshot";
        case CandidateRejection::noIPv4Service: return "noIPv4Service";
    }
    return "systemSnapshotUnavailable";
}

void CandidateDiagnostics::reject(CandidateRejection reason, unsigned count) {
    auto& previous = rejections[reason];
    previous = std::min(4096u, std::min(4096u, previous) + std::min(4096u, count));
}

CandidateDiagnostics anonymous_diagnostics(const CandidateDiagnostics& diagnostics) {
    CandidateDiagnostics safe;
    safe.interface_count = std::min<std::size_t>(4096, diagnostics.interface_count);
    safe.candidate_count = std::min<std::size_t>(4096, diagnostics.candidate_count);
    for (const auto reason : {CandidateRejection::systemSnapshotUnavailable, CandidateRejection::duplicateInterface,
            CandidateRejection::unsupportedInterface, CandidateRejection::inactiveLink, CandidateRejection::missingInterfaceIndex,
            CandidateRejection::missingSourceAddress, CandidateRejection::invalidSourceAddress, CandidateRejection::invalidSubnet,
            CandidateRejection::invalidGateway, CandidateRejection::missingDNS, CandidateRejection::invalidDNS,
            CandidateRejection::missingDescriptor, CandidateRejection::missingDefaultRoute, CandidateRejection::conflictingDefaultRoutes,
            CandidateRejection::gatewayMismatch, CandidateRejection::associatedDNSMismatch, CandidateRejection::invalidDHCPServer,
            CandidateRejection::unstableSnapshot, CandidateRejection::noIPv4Service}) {
        const auto found = diagnostics.rejections.find(reason);
        if (found != diagnostics.rejections.end() && found->second) safe.reject(reason, found->second);
    }
    return safe;
}

CandidateReport candidate_report(const std::vector<Attachment>& evidence) {
    CandidateReport report;
    report.diagnostics.interface_count = std::min<std::size_t>(4096, evidence.size());
    if (evidence.empty()) report.diagnostics.reject(CandidateRejection::noIPv4Service);
    std::map<std::string, unsigned> counts;
    for (const auto& a : evidence) ++counts[a.route.interface_key];
    for (const auto& a : evidence) {
        Route r = a.route;
        const auto reject = [&](CandidateRejection reason) { report.diagnostics.reject(reason); };
        if (!a.physical || (r.descriptor.kind != "wifi" && r.descriptor.kind != "ethernet")) { reject(CandidateRejection::unsupportedInterface); continue; }
        if (!a.up) { reject(CandidateRejection::inactiveLink); continue; }
        if (r.interface_key.empty() || !r.interface_index || !r.interface_luid) { reject(CandidateRejection::missingInterfaceIndex); continue; }
        if (counts[r.interface_key] != 1) { reject(CandidateRejection::duplicateInterface); continue; }
        if (a.collection_failure) { reject(*a.collection_failure); continue; }
        if (r.source_ipv4.empty() || r.addresses.empty()) { reject(CandidateRejection::missingSourceAddress); continue; }
        if (!usable_ipv4(r.source_ipv4) || r.addresses.size() > 16 || r.addresses.front() != r.source_ipv4 ||
            !std::all_of(r.addresses.begin(), r.addresses.end(), usable_ipv4)) { reject(CandidateRejection::invalidSourceAddress); continue; }
        if (!usable_ipv4(r.gateway_ipv4)) { reject(CandidateRejection::invalidGateway); continue; }
        if (!r.descriptor.reliable()) { reject(CandidateRejection::missingDescriptor); continue; }
        if (r.dns_servers.empty()) { reject(CandidateRejection::missingDNS); continue; }
        if (r.dns_servers.size() > 8 || !std::all_of(r.dns_servers.begin(), r.dns_servers.end(), usable_ipv4)) {
            reject(CandidateRejection::invalidDNS); continue;
        }
        const auto net = r.descriptor.raw.find("net"), gwip = r.descriptor.raw.find("gwip");
        if (net == r.descriptor.raw.end() || !source_in_network(r.source_ipv4, net->second)) { reject(CandidateRejection::invalidSubnet); continue; }
        if (gwip == r.descriptor.raw.end() || gwip->second != r.gateway_ipv4) { reject(CandidateRejection::gatewayMismatch); continue; }
        auto defaults = a.bound_default_gateways;
        canonical(defaults);
        if (defaults.empty()) { reject(CandidateRejection::missingDefaultRoute); continue; }
        if (defaults.size() != 1) { reject(CandidateRejection::conflictingDefaultRoutes); continue; }
        if (defaults.front() != r.gateway_ipv4) { reject(CandidateRejection::gatewayMismatch); continue; }
        canonical(r.dns_servers);
        std::string dns;
        for (const auto& d : r.dns_servers) dns += (dns.empty() ? "" : ",") + d;
        const auto associated = r.descriptor.raw.find("dns");
        if (associated == r.descriptor.raw.end() || associated->second != dns) { reject(CandidateRejection::associatedDNSMismatch); continue; }
        canonical(r.addresses);
        r.addresses.erase(std::find(r.addresses.begin(), r.addresses.end(), r.source_ipv4));
        r.addresses.insert(r.addresses.begin(), r.source_ipv4);
        std::string material;
        field(material, r.interface_key); field(material, std::to_string(r.interface_index)); field(material, std::to_string(r.interface_luid));
        field(material, r.source_ipv4); field(material, r.gateway_ipv4); field(material, r.descriptor.kind);
        for (const auto& v : r.addresses) field(material, v);
        field(material, "dns"); for (const auto& v : r.dns_servers) field(material, v);
        field(material, "descriptor"); for (const auto& [key, value] : r.descriptor.raw) { field(material, key); field(material, value); }
        r.identity = sha256_hex(material);
        report.routes.push_back(std::move(r));
    }
    std::sort(report.routes.begin(), report.routes.end(), [](const Route& a, const Route& b) {
        if (a.is_system_primary != b.is_system_primary) return a.is_system_primary;
        return a.interface_key < b.interface_key;
    });
    report.diagnostics.candidate_count = std::min<std::size_t>(4096, report.routes.size());
    return report;
}

CandidateReport stable_candidate_report(const std::vector<Attachment>& first, const std::vector<Attachment>& second) {
    const auto old = candidate_report(first);
    auto current = candidate_report(second);
    for (const auto& [reason, count] : old.diagnostics.rejections)
        current.diagnostics.rejections[reason] = std::max(current.diagnostics.rejections[reason], count);
    const auto before = current.routes.size();
    current.routes.erase(std::remove_if(current.routes.begin(), current.routes.end(), [&](const Route& r) {
        return std::none_of(old.routes.begin(), old.routes.end(), [&](const Route& previous) { return previous == r; });
    }), current.routes.end());
    if (before != current.routes.size()) current.diagnostics.reject(CandidateRejection::unstableSnapshot,
        static_cast<unsigned>(std::min<std::size_t>(4096, before - current.routes.size())));
    current.diagnostics.candidate_count = std::min<std::size_t>(4096, current.routes.size());
    return current;
}

std::vector<Route> candidates(const std::vector<Attachment>& evidence) { return candidate_report(evidence).routes; }
std::vector<Route> stable_candidates(const std::vector<Attachment>& first, const std::vector<Attachment>& second) {
    return stable_candidate_report(first, second).routes;
}

bool physical_hardware_confirmed(const HardwareEvidence& e) {
    return e.readable && e.expected_luid && e.expected_index && e.expected_luid == e.actual_luid &&
        e.expected_index == e.actual_index && e.up && e.hardware_interface && !e.filter_interface && !e.endpoint_interface;
}

bool other_physical_link_active(const OtherLinkEvidence& e) {
    if (!e.up || e.known_virtual) return false;
    if (e.details_readable && e.media_disconnected) return false;
    if (e.details_readable && (e.hardware_interface || e.connector_present) && e.media_connected) return true;
    // An UP unknown attachment with a usable source may already carry the user's
    // network. Preserve it even when GetIfEntry2 cannot confirm its hardware type.
    // A dormant virtual/unknown interface with only link-local addresses does not
    // block finding Wi-Fi. Known virtuals were excluded before this fallback.
    return e.usable_source;
}

Error::Error(Failure value) : std::runtime_error(value == Failure::cancelled ? "physical_cancelled" :
    value == Failure::timeout ? "physical_timeout" : value == Failure::invalidResponse ? "physical_invalid_response" :
    value == Failure::security ? "physical_security" : "physical_unavailable"), reason(value) {}

bool allowed_host(std::string_view host) {
    return host == "net.szu.edu.cn" || host == "www.apple.com" || host == "www.msftconnecttest.com";
}
Packet dns_query(std::string_view host, std::uint16_t transaction) {
    if (!allowed_host(host)) throw Error(Failure::unavailable);
    Packet p{static_cast<std::uint8_t>(transaction >> 8), static_cast<std::uint8_t>(transaction & 255), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    std::size_t start = 0;
    while (start < host.size()) {
        const auto stop = host.find('.', start), end = stop == std::string_view::npos ? host.size() : stop;
        p.push_back(static_cast<std::uint8_t>(end - start));
        for (std::size_t i = start; i < end; ++i) p.push_back(static_cast<std::uint8_t>(host[i]));
        start = end + 1;
    }
    p.insert(p.end(), {0, 0, 1, 0, 1}); return p;
}

std::vector<std::string> dns_parse(const Packet& p, std::string_view host, std::uint16_t transaction) {
    std::size_t offset = 0;
    if (!allowed_host(host) || !exact_question(p, host, transaction, offset) || (u16(p, 2) & 0x024f)) throw Error(Failure::invalidResponse);
    const auto answer_count = u16(p, 6);
    const unsigned total = unsigned(answer_count) + u16(p, 8) + u16(p, 10);
    if (total > 256) throw Error(Failure::invalidResponse);
    struct Answer { std::string owner, cname, address; };
    std::vector<Answer> answers;
    for (unsigned record = 0; record < total; ++record) {
        const auto owner = name(p, offset);
        if (offset + 10 > p.size()) throw Error(Failure::invalidResponse);
        const auto type = u16(p, offset), klass = u16(p, offset + 2), length = u16(p, offset + 8);
        offset += 10;
        const auto end = offset + length;
        if (end > p.size()) throw Error(Failure::invalidResponse);
        if (record < answer_count && klass == 1) {
            if (type == 1) {
                if (length != 4) throw Error(Failure::invalidResponse);
                const auto address = std::to_string(p[offset]) + "." + std::to_string(p[offset + 1]) + "." +
                    std::to_string(p[offset + 2]) + "." + std::to_string(p[offset + 3]);
                if (!usable_ipv4(address)) throw Error(Failure::invalidResponse);
                answers.push_back({owner, {}, address});
            } else if (type == 5) {
                auto cursor = offset;
                const auto cname = name(p, cursor);
                if (cursor != end || cname.empty()) throw Error(Failure::invalidResponse);
                answers.push_back({owner, cname, {}});
            }
        }
        offset = end;
    }
    if (offset != p.size()) throw Error(Failure::invalidResponse);
    std::string current(host);
    std::set<std::string> visited;
    for (int hop = 0; hop < 16; ++hop) {
        if (!visited.insert(current).second) throw Error(Failure::invalidResponse);
        std::set<std::string> cnames, addresses;
        for (const auto& a : answers) if (a.owner == current) {
            if (!a.cname.empty()) cnames.insert(a.cname);
            if (!a.address.empty()) addresses.insert(a.address);
        }
        if (cnames.size() > 1 || (!cnames.empty() && !addresses.empty())) throw Error(Failure::invalidResponse);
        if (!addresses.empty()) return {addresses.begin(), addresses.end()};
        if (cnames.empty()) throw Error(Failure::invalidResponse);
        current = *cnames.begin();
    }
    throw Error(Failure::invalidResponse);
}

double dns_response_ttl(const Packet& packet, std::string_view host, std::uint16_t transaction) {
    (void)dns_parse(packet, host, transaction);
    std::size_t offset = 0;
    if (!exact_question(packet, host, transaction, offset)) throw Error(Failure::invalidResponse);
    const unsigned total = unsigned(u16(packet, 6)) + u16(packet, 8) + u16(packet, 10);
    std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
    for (unsigned record = 0; record < total; ++record) {
        (void)name(packet, offset);
        if (offset + 10 > packet.size()) throw Error(Failure::invalidResponse);
        const auto type = u16(packet, offset), klass = u16(packet, offset + 2);
        const auto ttl = (std::uint32_t(packet[offset + 4]) << 24) | (std::uint32_t(packet[offset + 5]) << 16) |
            (std::uint32_t(packet[offset + 6]) << 8) | std::uint32_t(packet[offset + 7]);
        // OPT stores extended flags in this field, rather than a cache lifetime.
        if (klass == 1 && type != 41) minimum = std::min(minimum, ttl);
        offset += 10 + u16(packet, offset + 8);
        if (offset > packet.size()) throw Error(Failure::invalidResponse);
    }
    if (offset != packet.size()) throw Error(Failure::invalidResponse);
    return minimum;
}

std::vector<std::string> dns_exchange(std::string_view host, std::uint16_t transaction, std::size_t servers,
                                    double deadline, const ExchangeHooks& h) {
    if (h.cancelled()) throw Error(Failure::cancelled);
    if (!allowed_host(host) || !servers || servers > 8 || !std::isfinite(deadline)) throw Error(Failure::unavailable);
    const auto query = dns_query(host, transaction);
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < servers; ++i) pending.push_back(i);
    bool invalid = false;
    std::size_t attempt = 0;
    const double started = h.now(), offsets[] = {0, 0.25, 0.75};
    const auto erase = [&](std::size_t i) { pending.erase(std::remove(pending.begin(), pending.end(), i), pending.end()); };
    while (true) {
        if (h.cancelled()) throw Error(Failure::cancelled);
        if (h.now() >= deadline) break;
        if (attempt < 3 && h.now() >= started + offsets[attempt]) {
            ++attempt;
            const auto sending = pending;
            for (const auto i : sending) {
                if (h.cancelled()) throw Error(Failure::cancelled);
                if (h.now() >= deadline) throw Error(Failure::timeout);
                if (!h.send(i, query)) erase(i);
                if (h.cancelled()) throw Error(Failure::cancelled);
            }
        }
        if (pending.empty()) throw Error(invalid ? Failure::invalidResponse : Failure::unavailable);
        const auto next_send = attempt < 3 ? started + offsets[attempt] : deadline;
        const auto wait = std::min(0.05, std::min(next_send, deadline) - h.now());
        if (wait <= 0) continue;
        if (h.cancelled()) throw Error(Failure::cancelled);
        const auto events = h.receive(pending, wait);
        if (h.cancelled()) throw Error(Failure::cancelled);
        if (h.now() >= deadline) break;
        for (const auto& e : events) {
            if (std::find(pending.begin(), pending.end(), e.server) == pending.end()) continue;
            if (e.kind == ExchangeEvent::Kind::failed) { erase(e.server); continue; }
            std::vector<std::string> answer;
            switch (classify(e.data, host, transaction, answer)) {
            case Reply::ignored: invalid = true; break;
            case Reply::terminal: invalid = true; erase(e.server); break;
            case Reply::answer:
                if (h.cancelled()) throw Error(Failure::cancelled);
                if (h.now() >= deadline) throw Error(Failure::timeout);
                return answer;
            }
        }
        if (pending.empty()) throw Error(invalid ? Failure::invalidResponse : Failure::unavailable);
    }
    if (h.cancelled()) throw Error(Failure::cancelled);
    throw Error(invalid ? Failure::invalidResponse : Failure::timeout);
}
}  // namespace szu::physical
