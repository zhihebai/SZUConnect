#include "szu/environment.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "szu/json.hpp"
#include "szu/types.hpp"
#include "szu/portal_wire.hpp"

namespace szu {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ';' || c == ' ') {
            const std::string t = trim(current);
            if (!t.empty()) out.push_back(t);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::string t = trim(current);
    if (!t.empty()) out.push_back(t);
    return out;
}

std::optional<std::uint32_t> ipv4(std::string_view text) {
    std::uint32_t value = 0;
    int parts = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t dot = text.find('.', start);
        if (dot == std::string_view::npos) dot = text.size();
        const std::string_view part = text.substr(start, dot - start);
        if (!all_digits(part) || part.size() > 3) return std::nullopt;
        const int n = std::stoi(std::string(part));
        if (n > 255) return std::nullopt;
        value = (value << 8) | static_cast<std::uint32_t>(n);
        ++parts;
        if (dot == text.size()) break;
        start = dot + 1;
    }
    if (parts != 4) return std::nullopt;
    return value;
}

bool glob(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Unicode-aware enough for the contract: ASCII whitespace plus U+00A0 / U+3000 (see trim()).
bool has_control(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) return true;
        if (c == 0xC2 && i + 1 < s.size()) {
            const auto d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0x80 && d <= 0x9F) return true;
        }
    }
    return false;
}

bool has_format_control(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = first;
        const int extra = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC0 ? 1 : 0;
        if (extra) cp = first & (extra == 3 ? 7 : extra == 2 ? 15 : 31);
        for (int j = 0; j < extra; ++j) cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        if (cp == 0xAD || (cp >= 0x600 && cp <= 0x605) || cp == 0x61C || cp == 0x6DD || cp == 0x70F ||
            (cp >= 0x890 && cp <= 0x891) || cp == 0x8E2 || cp == 0x180E || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) ||
            cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) || cp == 0x110BD || cp == 0x110CD ||
            (cp >= 0x13430 && cp <= 0x1345F) || cp == 0x1BCA0 || cp == 0x1BCA1 || cp == 0x1BCA2 || cp == 0x1BCA3 ||
            (cp >= 0x1D173 && cp <= 0x1D17A) || cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F)) return true;
    }
    return false;
}

}  // namespace

// ---------------- website ----------------
namespace website {

bool is_campus_redirect(std::string_view location, std::string_view request_url) {
    if (location.empty() || location.size() > 2048 || !json::valid_utf8(location) ||
        has_control(location) || has_format_control(location) || location.find('\\') != std::string_view::npos) return false;
    std::string destination(location);
    const auto first_delimiter = location.find_first_of("/?#");
    const auto colon = location.find(':');
    if (location.starts_with("//")) {
        const auto base = wire::parse_url(request_url);
        if (!base) return false;
        destination = base->scheme + ":" + destination;
    } else if (colon == std::string_view::npos || colon > first_delimiter) {
        const auto base = wire::parse_url(request_url);
        if (!base || base->has_userinfo || (base->scheme != "http" && base->scheme != "https")) return false;
        std::string origin = base->scheme + "://" + base->host;
        if (base->port) origin += ":" + std::to_string(*base->port);
        if (location.starts_with('/')) destination = origin + destination;
        else if (location.starts_with('?') || location.starts_with('#')) destination = origin + base->path + destination;
        else {
            const auto slash = base->path.rfind('/');
            destination = origin + (slash == std::string::npos ? "/" : base->path.substr(0, slash + 1)) + destination;
        }
    }
    const auto target = wire::parse_url(destination);
    if (!target || target->has_userinfo) return false;
    const int port = target->port.value_or(target->scheme == "https" ? 443 : 80);
    if (target->scheme == "https" && target->host == "net.szu.edu.cn" && port == 443)
        return target->path.empty() || target->path == "/" || target->path == "/srun_portal_pc";
    if (target->scheme == "http" && target->host == "172.30.255.42" && target->port == 801)
        return target->path.empty() || target->path == "/" || target->path == "/eportal";
    return false;
}

Normalized normalize(std::string_view input) {
    const std::string trimmed = trim(input);
    if (trimmed.empty()) return {"", "empty"};
    if (trimmed.size() > 2048) return {"", "tooLong"};
    if (has_control(trimmed) || trimmed.find('\\') != std::string::npos) return {"", "invalid"};
    std::string value;
    if (trimmed.find("://") != std::string::npos) {
        value = trimmed;
    } else if (trimmed.rfind("//", 0) == 0) {
        value = "https:" + trimmed;
    } else {
        if (trimmed.find(':') != std::string::npos && trimmed.front() != '[') {
            std::string suffix = trimmed.substr(trimmed.find(':') + 1);
            suffix = suffix.substr(0, suffix.find('/'));
            if (!all_digits(suffix)) return {"", "unsupportedScheme"};
        }
        value = "https://" + trimmed;
    }
    const std::size_t sep = value.find("://");
    const std::string scheme_raw = value.substr(0, sep);
    const bool scheme_ok = !scheme_raw.empty() && ((scheme_raw[0] >= 'A' && scheme_raw[0] <= 'Z') || (scheme_raw[0] >= 'a' && scheme_raw[0] <= 'z')) &&
                           std::all_of(scheme_raw.begin(), scheme_raw.end(), [](char c) {
                               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-';
                           });
    if (!scheme_ok) return {"", "unsupportedScheme"};
    const std::string scheme = lower(scheme_raw);
    if (scheme != "http" && scheme != "https") return {"", "unsupportedScheme"};
    const std::string rest = value.substr(sep + 3);
    std::size_t end = rest.size();
    for (const char mark : {'/', '?', '#'}) {
        const std::size_t index = rest.find(mark);
        if (index != std::string::npos) end = std::min(end, index);
    }
    const std::string authority = rest.substr(0, end);
    const std::string tail = rest.substr(end);
    if (value.find(' ') != std::string::npos) return {"", "unsupportedScheme"};
    if (authority.find('@') != std::string::npos) return {"", "sensitiveComponents"};
    if (tail.find('?') != std::string::npos || tail.find('#') != std::string::npos) return {"", "sensitiveComponents"};
    if (!authority.empty() && authority.front() == '[') return {"", "invalid"};
    std::string host = authority;
    std::optional<long> port;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        if (!all_digits(port_text) || port_text.size() > 9) return {"", "invalid"};
        port = std::stol(port_text);
    }
    if (host.empty()) return {"", "invalid"};
    for (const char c : host) {
        const auto u = static_cast<unsigned char>(c);
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (u > 0x7E || !allowed) return {"", "invalid"};
    }
    if (port && (*port < 1 || *port > 65535)) return {"", "invalid"};
    std::string normalized = scheme + "://" + lower(host);
    if (port) normalized += ":" + std::to_string(*port);
    normalized += tail;
    return {normalized, ""};
}

std::string_view classify_status(int code) {
    if (code >= 200 && code <= 299) return "accessible";
    if (code >= 300 && code <= 399) return "redirected";
    if (code == 401 || code == 407) return "authenticationRequired";
    if (code == 403) return "forbidden";
    return "httpStatus";
}

}  // namespace website

// ---------------- proxy ----------------
namespace proxy {

bool pattern_matches(std::string_view pattern, std::string_view host) {
    const std::string p = lower(trim(pattern));
    const std::string h = lower(host);
    if (p.empty()) return false;
    if (p == "<local>") return h.find('.') == std::string::npos && h.find(':') == std::string::npos;
    if (const std::size_t slash = p.find('/'); slash != std::string::npos) {
        const std::string base = p.substr(0, slash);
        const std::string bits = p.substr(slash + 1);
        if (!all_digits(bits) || bits.size() > 3) return false;
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true) {
            const std::size_t dot = base.find('.', start);
            parts.push_back(base.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (parts.empty() || parts.size() > 4) return false;
        std::uint32_t network = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            int octet = 0;
            if (i < parts.size()) {
                if (!all_digits(parts[i]) || parts[i].size() > 3) return false;
                octet = std::stoi(parts[i]);
                if (octet > 255) return false;
            }
            network = (network << 8) | static_cast<std::uint32_t>(octet);
        }
        const int prefix = std::stoi(bits);
        if (prefix > 32) return false;
        const auto address = ipv4(h);
        if (!address) return false;
        const std::uint32_t mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
        return (*address & mask) == (network & mask);
    }
    if (p.rfind("*.", 0) == 0) {
        const std::string suffix = p.substr(1);
        return h.size() >= suffix.size() && h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    if (p.front() == '.') {
        return h == p.substr(1) || (h.size() >= p.size() && h.compare(h.size() - p.size(), p.size(), p) == 0);
    }
    if (p.find('*') != std::string::npos) return glob(p, h);
    return h == p;
}

Summary summarize(const Snapshot& s) {
    if (!s.readable) return {"unknown", {}, "unknown"};
    std::vector<std::string> kinds;
    if (s.http) kinds.push_back("http");
    if (s.https) kinds.push_back("https");
    if (s.socks) kinds.push_back("socks");
    if (s.pac) kinds.push_back("pac");
    if (s.auto_discovery) kinds.push_back("autoDiscovery");
    if (kinds.empty()) return {"off", {}, "notApplicable"};
    if (!s.http && !s.https && !s.socks) return {"on", kinds, "unknown"};
    int covered = 0;
    for (const auto host : kCampusHosts) {
        if (std::any_of(s.exceptions.begin(), s.exceptions.end(), [&](const std::string& p) { return pattern_matches(p, host); })) ++covered;
    }
    const int total = static_cast<int>(std::size(kCampusHosts));
    return {"on", kinds, covered == total ? "all" : (covered ? "partial" : "none")};
}

Snapshot parse_windows(const WindowsRaw& raw) {
    Snapshot s;
    if (!raw.readable) return s;
    s.readable = true;
    s.pac = !raw.auto_config_url.empty();
    s.auto_discovery = raw.auto_detect;
    for (const std::string& entry : split_list(raw.proxy)) {
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            s.http = true;
            s.https = true;
            continue;
        }
        const std::string scheme = lower(trim(entry.substr(0, eq)));
        if (trim(entry.substr(eq + 1)).empty()) continue;
        if (scheme == "http") s.http = true;
        else if (scheme == "https") s.https = true;
        else if (scheme == "socks") s.socks = true;
    }
    s.exceptions = split_list(raw.bypass);
    return s;
}

}  // namespace proxy

// ---------------- compatibility diagnosis ----------------
namespace compat {

Verdict diagnose(const Input& in) {
    const std::string R = "reachable";
    for (const std::string* v : {&in.campus_direct, &in.public_direct, &in.campus_system, &in.public_system})
        if (*v == "cancelled") return {"cancelled", {}, false};
    if (in.campus_direct == "tls") return {"campusTLS", {"checkClock", "checkInterception"}, false};
    if (in.campus_direct == R && in.public_direct == R) return {"allReachable", {"nothingToFix"}, false};
    if (in.campus_direct == R) {
        if (in.public_system == R) return {"publicOnlyViaProxy", {"loginFirst", "proxyMayHideLogin"}, false};
        return {"loginLikelyNeeded", {"loginFirst"}, false};
    }
    if (in.tunnel == "detected") return {"campusBlockedByTunnel", {"addDirectRules", "tunBypassCampus", "retryAfterRules"}, true};
    if (in.campus_system == R) return {"campusOnlyViaProxy", {"addDirectRules", "checkRouting"}, true};
    if (in.proxy == "on") return {"campusUnreachableWithProxy", {"confirmCampusNetwork", "addDirectRules"}, true};
    Verdict v{in.public_direct == R ? "notOnCampusNetwork" : "campusUnreachable", {"confirmCampusNetwork"}, false};
    if (v.verdict == "campusUnreachable") v.recommendations.push_back("checkCable");
    if (in.tunnel == "unknown") v.recommendations.push_back("tunnelStateUnknown");
    return v;
}

std::string rules_text() { return std::string(kDirectRules[0]) + "\n" + std::string(kDirectRules[1]); }

}  // namespace compat
}  // namespace szu
