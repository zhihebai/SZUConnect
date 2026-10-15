// Tunnel-route evidence (port of tools/reference/route_reference.py / macOS TunnelRouteSnapshot.classify).
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

#include "szu/environment.hpp"

namespace szu::routes {
namespace {

using Bytes = std::array<std::uint8_t, 16>;

std::optional<Bytes> parse_v4(std::string_view text) {
    Bytes out{};
    int part = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = text.find('.', start);
        const std::string_view piece = text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (piece.empty() || piece.size() > 3 || part > 3 ||
            !std::all_of(piece.begin(), piece.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        const int value = std::stoi(std::string(piece));
        if (value > 255) return std::nullopt;
        out[static_cast<std::size_t>(part++)] = static_cast<std::uint8_t>(value);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (part != 4) return std::nullopt;
    return out;
}

std::optional<Bytes> parse_v6(std::string_view text) {
    std::array<std::uint16_t, 8> groups{};
    std::vector<std::uint16_t> head, tail;
    const std::size_t gap = text.find("::");
    auto parse_groups = [](std::string_view part, std::vector<std::uint16_t>& out) -> bool {
        if (part.empty()) return true;
        std::size_t start = 0;
        while (true) {
            const std::size_t colon = part.find(':', start);
            const std::string_view g = part.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
            if (g.empty() || g.size() > 4) return false;
            std::uint16_t value = 0;
            for (const char c : g) {
                value = static_cast<std::uint16_t>(value << 4);
                if (c >= '0' && c <= '9') value = static_cast<std::uint16_t>(value | (c - '0'));
                else if (c >= 'a' && c <= 'f') value = static_cast<std::uint16_t>(value | (c - 'a' + 10));
                else if (c >= 'A' && c <= 'F') value = static_cast<std::uint16_t>(value | (c - 'A' + 10));
                else return false;
            }
            out.push_back(value);
            if (colon == std::string_view::npos) return true;
            start = colon + 1;
        }
    };
    if (gap == std::string_view::npos) {
        if (!parse_groups(text, head) || head.size() != 8) return std::nullopt;
    } else {
        if (!parse_groups(text.substr(0, gap), head) || !parse_groups(text.substr(gap + 2), tail) || head.size() + tail.size() > 7)
            return std::nullopt;
    }
    for (std::size_t i = 0; i < head.size(); ++i) groups[i] = head[i];
    for (std::size_t i = 0; i < tail.size(); ++i) groups[8 - tail.size() + i] = tail[i];
    Bytes out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 2] = static_cast<std::uint8_t>(groups[i] >> 8);
        out[i * 2 + 1] = static_cast<std::uint8_t>(groups[i] & 0xFF);
    }
    return out;
}

bool in_v4(const Bytes& raw, int prefix, std::uint32_t network, int bits) {
    if (prefix < bits) return false;
    const std::uint32_t address = (std::uint32_t{raw[0]} << 24) | (std::uint32_t{raw[1]} << 16) | (std::uint32_t{raw[2]} << 8) | raw[3];
    const std::uint32_t mask = bits == 0 ? 0u : (0xFFFFFFFFu << (32 - bits));
    return (address & mask) == network;
}

bool non_local(int family, const Bytes& raw, int prefix) {
    if (family == 4) {
        if (prefix == 0) return raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0;
        return !(prefix >= 8 && (raw[0] == 0 || raw[0] == 127)) && !(prefix >= 16 && raw[0] == 169 && raw[1] == 254) &&
               !(prefix >= 4 && raw[0] >= 224);
    }
    const bool all_zero = std::all_of(raw.begin(), raw.end(), [](std::uint8_t b) { return b == 0; });
    if (prefix == 0) return all_zero;
    const bool head_zero = std::all_of(raw.begin(), raw.end() - 1, [](std::uint8_t b) { return b == 0; });
    return !(prefix >= 10 && raw[0] == 0xFE && (raw[1] & 0xC0) == 0x80) && !(prefix >= 8 && raw[0] == 0xFF) &&
           !(prefix == 128 && head_zero && raw[15] <= 1);
}

bool public_evidence(int family, const Bytes& raw, int prefix) {
    if (family == 4) {
        struct Special { std::uint32_t net; int bits; };
        static const Special specials[] = {
            {0x00000000u, 8}, {0x0A000000u, 8}, {0x64400000u, 10}, {0x7F000000u, 8}, {0xA9FE0000u, 16}, {0xAC100000u, 12},
            {0xC0000000u, 24}, {0xC0000200u, 24}, {0xC0A80000u, 16}, {0xC6120000u, 15}, {0xC6336400u, 24},
            {0xCB007100u, 24}, {0xE0000000u, 4}, {0xF0000000u, 4}};
        for (const auto& s : specials)
            if (in_v4(raw, prefix, s.net, s.bits)) return false;
        return true;
    }
    const int bits = std::min(prefix, 3);
    const int mask = bits == 0 ? 0 : ((0xFF << (8 - bits)) & 0xFF);
    if ((raw[0] & mask) != (0x20 & mask)) return false;
    const bool documentation = raw[0] == 0x20 && raw[1] == 0x01 && raw[2] == 0x0D && raw[3] == 0xB8;
    return !(prefix >= 32 && documentation);
}

bool has(const std::vector<std::string>& flags, std::string_view flag) {
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

}  // namespace

bool is_tunnel_name(std::string_view name) {
    for (const std::string_view prefix : {"utun", "tun", "tap", "ppp", "ipsec"}) {
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix &&
            std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return true;
    }
    return false;
}

Evidence classify(const std::vector<Route>& routes, const std::vector<Interface>& interfaces, std::string_view view) {
    static const std::string_view excluded[] = {"reject", "blackhole", "cloned", "llinfo", "local", "broadcast", "multicast", "condemned", "dead"};
    Evidence evidence;
    for (const Route& route : routes) {
        if (!has(route.flags, "up")) continue;
        if (std::any_of(std::begin(excluded), std::end(excluded), [&](std::string_view f) { return has(route.flags, f); })) continue;
        const auto iface = std::find_if(interfaces.begin(), interfaces.end(), [&](const Interface& i) { return i.name == route.interface_name; });
        if (iface == interfaces.end() || !iface->up || !iface->running) continue;
        const bool tunnel = view == "mac" ? is_tunnel_name(route.interface_name) : iface->kind == "tunnel";
        if (!tunnel) continue;
        const auto raw = route.family == 4 ? parse_v4(route.destination) : parse_v6(route.destination);
        if (!raw || !non_local(route.family, *raw, route.prefix)) continue;
        bool counts;
        if (has(route.flags, "scoped")) counts = route.prefix > 0 && public_evidence(route.family, *raw, route.prefix);
        else if (!has(route.flags, "gateway")) counts = route.prefix <= 1 || public_evidence(route.family, *raw, route.prefix);
        else counts = true;
        if (counts) ++evidence.count;
    }
    evidence.tunnel = evidence.count > 0;
    return evidence;
}

}  // namespace szu::routes
