#include "szu/portal_wire.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace szu::wire {
namespace {

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

bool is_regex_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim_ascii(std::string_view text) {
    std::size_t b = 0, e = text.size();
    while (b < e && is_regex_space(text[b])) ++b;
    while (e > b && is_regex_space(text[e - 1])) --e;
    return std::string(text.substr(b, e - b));
}

}  // namespace

std::optional<ParsedUrl> parse_url(std::string_view url) {
    ParsedUrl out;
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return std::nullopt;
    out.scheme = ascii_lower(url.substr(0, scheme_end));
    std::string_view rest = url.substr(scheme_end + 3);
    std::size_t authority_end = rest.find_first_of("/?#");
    if (authority_end == std::string_view::npos) authority_end = rest.size();
    std::string_view authority = rest.substr(0, authority_end);
    std::string_view tail = rest.substr(authority_end);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        out.has_userinfo = true;
        authority = authority.substr(at + 1);
    }
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        const std::string_view port_text = authority.substr(colon + 1);
        if (port_text.empty() || port_text.size() > 5 ||
            !std::all_of(port_text.begin(), port_text.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        out.port = std::stoi(std::string(port_text));
        authority = authority.substr(0, colon);
    }
    out.host = ascii_lower(authority);
    if (out.host.empty()) return std::nullopt;
    const std::size_t fragment = tail.find('#');
    if (fragment != std::string_view::npos) {
        out.has_fragment = true;
        tail = tail.substr(0, fragment);
    }
    const std::size_t question = tail.find('?');
    if (question != std::string_view::npos) {
        out.has_query = true;
        out.query = std::string(tail.substr(question + 1));
        tail = tail.substr(0, question);
    }
    out.path = std::string(tail);
    return out;
}

bool is_allowed(std::string_view url) {
    const auto parsed = parse_url(url);
    if (!parsed || parsed->has_userinfo || parsed->has_fragment) return false;
    const auto& u = *parsed;
    auto port_is = [&](std::initializer_list<int> ports) {
        if (!u.port) return true;
        return std::find(ports.begin(), ports.end(), *u.port) != ports.end();
    };
    if (u.scheme == "https" && u.host == "net.szu.edu.cn" && port_is({443})) {
        return ((u.path == "/" || u.path == "/srun_portal_pc") && !u.has_query) ||
               u.path == "/cgi-bin/get_challenge" || u.path == "/cgi-bin/srun_portal";
    }
    if (u.scheme == "http" && u.host == "172.30.255.42" && u.port && *u.port == 801) {
        return ((u.path == "/" || u.path == "/eportal") && !u.has_query) || u.path == "/eportal/portal/login";
    }
    if (u.scheme == "https" && u.host == "www.apple.com" && port_is({443})) {
        return u.path == "/library/test/success.html" && !u.has_query;
    }
    if (u.scheme == "http" && u.host == "www.msftconnecttest.com" && port_is({80})) {
        return u.path == "/connecttest.txt" && !u.has_query;
    }
    return false;
}

std::string percent_encode(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::optional<std::string> make_url(std::string_view base, std::string_view path, const Query& query) {
    std::string url(base);
    url += path;
    if (!query.empty()) {
        url.push_back('?');
        for (std::size_t i = 0; i < query.size(); ++i) {
            if (i) url.push_back('&');
            url += percent_encode(query[i].first);
            url.push_back('=');
            url += percent_encode(query[i].second);
        }
    }
    if (!is_allowed(url)) return std::nullopt;
    return url;
}

std::optional<json::Value> object_from(std::string_view body) {
    if (body.size() > kMaxResponseBytes || !json::valid_utf8(body)) return std::nullopt;
    std::string text = trim_ascii(body);
    if (text.empty() || text.front() != '{') {
        if (!text.empty() && text.back() == ';') text.pop_back();
        const std::size_t open = text.find('(');
        if (open == std::string::npos || text.empty() || text.back() != ')') return std::nullopt;
        std::string callback = text.substr(0, open);
        while (!callback.empty() && callback.back() == ' ') callback.pop_back();
        while (!callback.empty() && callback.front() == ' ') callback.erase(callback.begin());
        if (callback != "_" && callback != "jsonpReturn") return std::nullopt;
        text = text.substr(open + 1, text.size() - open - 2);
    }
    auto value = json::parse(text);
    if (!value || !value->is_object()) return std::nullopt;
    return value;
}

bool teaching_success(const json::Value& object) {
    return teaching_verdict(object) == LoginVerdict::accepted;
}

namespace {
bool is_failure_message(std::string_view raw) {
    static const std::string_view zh[] = {"未", "不", "非", "无", "没", "失败", "错误", "异常", "拒绝", "等待", "正在", "稍后"};
    static const std::string_view en[] = {"fail", "error", "denied", "invalid", "incorrect", "reject", "not ", "pending", "waiting"};
    const std::string lower = ascii_lower(raw);
    for (auto word : zh) if (contains(raw, word)) return true;
    for (auto word : en) if (contains(lower, word)) return true;
    return false;
}
}  // namespace

bool is_dormitory_success_message(std::string_view raw) {
    std::string text = trim(raw);
    auto ends_with = [&](std::string_view suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    bool stripped = true;
    while (stripped && !text.empty()) {
        stripped = false;
        for (std::string_view mark : {"！", "。", "～", "!", ".", "~", " "}) {
            if (ends_with(mark)) {
                text.erase(text.size() - mark.size());
                stripped = true;
                break;
            }
        }
    }
    const std::string lower = ascii_lower(text);
    return lower == "认证成功" || lower == "portal协议认证成功";
}

bool dormitory_success(const json::Value& object) {
    const std::string message = object.string_at("msg").value_or("");
    if (is_failure_message(message)) return false;
    if (is_dormitory_success_message(message)) return true;
    if (const json::Value* result = object.find("result"); result && result->is_number() && result->as_number() == 0) return true;
    return false;
}

LoginVerdict teaching_verdict(const json::Value& object) {
    // Existing IP session is not evidence that this request accepted the supplied password.
    for (const char* key : {"suc_msg", "error_msg", "error", "res"})
        if (object.string_at(key).value_or("") == "ip_already_online_error") return LoginVerdict::uncertain;
    bool positive = false, negative = false, invalid = false;
    for (const char* key : {"error", "res"}) {
        if (const json::Value* raw = object.find(key)) {
            if (!raw->is_string()) { invalid = true; continue; }
            const std::string& value = raw->as_string();
            if (value == "ok") positive = true;
            else if (!value.empty()) {
                const std::string lower = ascii_lower(value);
                if (lower == "pending" || lower == "waiting" || lower == "processing") invalid = true;
                else negative = true;
            }
        }
    }
    if (const json::Value* raw = object.find("st")) {
        if (raw->is_bool()) { if (raw->as_bool()) positive = true; else negative = true; }
        else if (raw->is_number()) {
            if (raw->as_number() == 1) positive = true;
            else if (raw->as_number() == 0) negative = true;
            else invalid = true;
        } else if (raw->is_string()) {
            if (raw->as_string() == "1") positive = true;
            else if (raw->as_string() == "0") negative = true;
            else invalid = true;
        } else invalid = true;
    }
    if (positive && (negative || invalid)) return LoginVerdict::uncertain;
    if (invalid) return LoginVerdict::uncertain;
    if (positive) return LoginVerdict::accepted;
    if (negative) return LoginVerdict::rejected;
    return LoginVerdict::uncertain;
}

LoginVerdict dormitory_verdict(const json::Value& object) {
    const json::Value* raw_message = object.find("msg");
    if (raw_message && !raw_message->is_string()) return LoginVerdict::uncertain;
    const std::string message = object.string_at("msg").value_or("");
    const std::string lower = ascii_lower(message);
    static const std::string_view failures[] = {"失败", "错误", "拒绝", "不存在", "无权限", "未认证成功",
                                                "fail", "error", "denied", "invalid", "incorrect", "reject", "not found"};
    bool failed = false;
    for (auto marker : failures) if (contains(lower, marker)) failed = true;
    const json::Value* result = object.find("result");
    if (result && (!result->is_number() || (result->as_number() != 0 && result->as_number() != 1)))
        return LoginVerdict::uncertain;
    const bool positive_result = result && result->is_number() && result->as_number() == 0;
    if (failed && positive_result) return LoginVerdict::uncertain;
    if (failed) return LoginVerdict::rejected;
    if (dormitory_success(object)) return LoginVerdict::accepted;
    return LoginVerdict::uncertain;
}

RejectionKind rejection_kind(const json::Value& object) {
    static const std::string_view fields[] = {"error_msg", "ploy_msg", "error", "res", "suc_msg", "msg", "message"};
    static const std::string_view password[] = {"e2553", "password is error", "password error", "ldap auth error",
                                                "密码错误", "密码不正确", "账号或密码错误", "用户名或密码错误"};
    static const std::string_view account[] = {"e2531", "user not found", "userid error", "no such user", "用户不存在",
                                               "账号不存在", "账户不存在"};
    static const std::string_view area[] = {"e2833", "dhcp table", "not in the dhcp", "不在认证范围", "ip地址不在", "ip 地址不在",
                                            "不允许在此区域", "nas_ip_error", "ac_id_error"};
    std::string joined;
    for (auto field : fields) {
        if (auto value = object.string_at(field); value && !value->empty()) {
            if (!joined.empty()) joined += " | ";
            joined += ascii_lower(*value);
        }
    }
    for (auto marker : password) if (contains(joined, marker)) return RejectionKind::password;
    for (auto marker : account) if (contains(joined, marker)) return RejectionKind::account;
    for (auto marker : area) if (contains(joined, marker)) return RejectionKind::areaMismatch;
    return RejectionKind::unspecified;
}

namespace {

bool ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool attr_boundary_char(char c) { return ident_char(c) || c == ':'; }

// Value after "key\s*[:=]\s*": "..." | '...' | unquoted run (may be empty, as in the Swift regex).
std::string read_value(std::string_view html, std::size_t& j) {
    if (j < html.size() && (html[j] == '"' || html[j] == '\'')) {
        const char quote = html[j];
        const std::size_t close = html.find(quote, j + 1);
        if (close != std::string_view::npos) {
            std::string value(html.substr(j + 1, close - j - 1));
            j = close + 1;
            return value;
        }
        return {};  // unterminated quote: the unquoted alternative matches the empty string
    }
    const std::size_t start = j;
    while (j < html.size() && !is_regex_space(html[j]) && std::string_view("&\"'<>;,)}]").find(html[j]) == std::string_view::npos) ++j;
    return std::string(html.substr(start, j - start));
}

// Values of attribute `name` inside one <input ...> tag (Swift attribute regex port).
std::vector<std::string> attribute_values(std::string_view tag, std::string_view name) {
    std::vector<std::string> values;
    const std::string lower = ascii_lower(tag);
    std::size_t pos = 0;
    while ((pos = lower.find(name, pos)) != std::string::npos) {
        if (pos > 0 && attr_boundary_char(lower[pos - 1])) { ++pos; continue; }
        std::size_t j = pos + name.size();
        while (j < tag.size() && is_regex_space(tag[j])) ++j;
        if (j >= tag.size() || tag[j] != '=') { ++pos; continue; }
        ++j;
        while (j < tag.size() && is_regex_space(tag[j])) ++j;
        if (j < tag.size() && (tag[j] == '"' || tag[j] == '\'')) {
            const char quote = tag[j];
            const std::size_t close = tag.find(quote, j + 1);
            if (close == std::string_view::npos) { ++pos; continue; }
            values.emplace_back(tag.substr(j + 1, close - j - 1));
            pos = close + 1;
            continue;
        }
        const std::size_t start = j;
        while (j < tag.size() && !is_regex_space(tag[j]) && std::string_view("\"'=<>`").find(tag[j]) == std::string_view::npos) ++j;
        if (j == start) { ++pos; continue; }
        values.emplace_back(tag.substr(start, j - start));
        pos = j;
    }
    return values;
}

}  // namespace

AcIdResult ac_id(std::string_view html) {
    AcIdResult result;
    if (html.size() > kMaxResponseBytes) { result.error = PortalError::responseTooLarge; return result; }
    if (!json::valid_utf8(html)) { result.error = PortalError::response; return result; }
    const std::string lower = ascii_lower(html);
    std::vector<std::string> candidates;
    std::size_t pos = 0;
    while (pos < lower.size()) {
        std::size_t token = 0;
        if (lower.compare(pos, 5, "ac_id") == 0) token = 5;
        else if (lower.compare(pos, 7, "\"ac_id\"") == 0 || lower.compare(pos, 7, "'ac_id'") == 0) token = 7;
        else if (lower.compare(pos, 4, "acid") == 0) token = 4;
        else if (lower.compare(pos, 6, "\"acid\"") == 0 || lower.compare(pos, 6, "'acid'") == 0) token = 6;
        if (!token || (pos > 0 && ident_char(lower[pos - 1]))) { ++pos; continue; }
        std::size_t j = pos + token;
        while (j < html.size() && is_regex_space(html[j])) ++j;
        if (j >= html.size() || (html[j] != ':' && html[j] != '=')) { ++pos; continue; }
        ++j;
        while (j < html.size() && is_regex_space(html[j])) ++j;
        candidates.push_back(read_value(html, j));
        pos = std::max(j, pos + 1);
    }
    // Both `acid` and `ac_id` from the fixed HTTPS page denote one controller.
    std::size_t tag = 0;
    while ((tag = lower.find("<input", tag)) != std::string::npos) {
        const std::size_t after = tag + 6;
        if (after < lower.size() && (std::isalnum(static_cast<unsigned char>(lower[after])) || lower[after] == '_')) { tag = after; continue; }
        const std::size_t close = lower.find('>', after);
        if (close == std::string::npos) break;
        const std::string_view body = html.substr(tag, close - tag + 1);
        const auto names = attribute_values(body, "name");
        if (std::find(names.begin(), names.end(), "ac_id") != names.end() ||
            std::find(names.begin(), names.end(), "acid") != names.end()) {
            const auto ids = attribute_values(body, "value");
            const bool one_name = !names.empty() && std::all_of(names.begin(), names.end(), [&](const std::string& n) { return n == names.front(); });
            if (!one_name || ids.empty()) { result.error = PortalError::protocolConflict; return result; }
            candidates.insert(candidates.end(), ids.begin(), ids.end());
        }
        tag = close + 1;
    }
    std::set<std::string> identifiers;
    for (const auto& value : candidates) {
        if (value.empty() || value.size() > 8 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            result.error = PortalError::protocolConflict;
            return result;
        }
        const long number = std::stol(value);
        if (number <= 0) { result.error = PortalError::protocolConflict; return result; }
        identifiers.insert(std::to_string(number));
    }
    if (identifiers.size() != 1) { result.error = PortalError::protocolConflict; return result; }
    result.value = *identifiers.begin();
    return result;
}

ChallengeResult challenge(const json::Value& object) {
    ChallengeResult result;
    std::vector<std::string> states;
    for (const char* key : {"error", "res"}) {
        const json::Value* raw = object.find(key);
        if (!raw) continue;
        if (!raw->is_string() || raw->as_string().empty()) { result.error = PortalError::protocolConflict; return result; }
        states.push_back(raw->as_string());
    }
    if (states.empty()) { result.error = PortalError::protocolConflict; return result; }
    if (std::find(states.begin(), states.end(), "ok") == states.end()) { result.error = PortalError::challengeRefused; return result; }
    if (!std::all_of(states.begin(), states.end(), [](const std::string& s) { return s == "ok"; })) {
        result.error = PortalError::protocolConflict;
        return result;
    }
    const auto token = object.string_at("challenge");
    const auto ip = object.string_at("client_ip");
    // Swift counts Characters for the IP limit; ASCII IPs make bytes and characters equal.
    if (!token || token->empty() || token->size() > 4096 || !ip || ip->empty() || ip->size() > 64) {
        result.error = PortalError::protocolConflict;
        return result;
    }
    result.challenge = *token;
    result.ip = *ip;
    return result;
}

bool is_connectivity_success(std::string_view body, int status) {
    return status == 200 && trim_ascii(body) == "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
}

bool is_microsoft_success(std::string_view body, int status) {
    return status == 200 && trim_ascii(body) == "Microsoft Connect Test";
}

bool is_campus_homepage(std::string_view body, int status, std::string_view url) {
    const auto parsed = parse_url(url);
    if (status != 200 || body.size() > kMaxResponseBytes || !parsed || parsed->has_query ||
        (parsed->path != "/" && parsed->path != "/srun_portal_pc") || !is_allowed(url) || !json::valid_utf8(body))
        return false;
    const std::string html = ascii_lower(body);
    if (!contains(html, "深圳大学") && !contains(html, "shenzhen university")) return false;
    const bool login = contains(html, "login") || contains(html, "登录");
    if (parsed->host == "net.szu.edu.cn") return contains(html, "srun") && login;
    if (parsed->host == "172.30.255.42") return contains(html, "drcom") && login;
    return false;
}

bool is_dormitory_homepage(std::string_view body, int status, std::string_view url, bool campus_confirmed) {
    const auto parsed = parse_url(url);
    if (!campus_confirmed || status != 200 || body.size() > kMaxResponseBytes || !parsed || parsed->host != "172.30.255.42" ||
        parsed->path != "/eportal" || parsed->has_query || !is_allowed(url) || !json::valid_utf8(body))
        return false;
    const std::string html = ascii_lower(body);
    std::size_t pos = 0;
    while ((pos = html.find("<title>", pos)) != std::string::npos) {
        std::size_t j = pos + 7;
        while (j < html.size() && is_regex_space(html[j])) ++j;
        if (html.compare(j, 7, "eportal") == 0) {
            j += 7;
            while (j < html.size() && is_regex_space(html[j])) ++j;
            if (html.compare(j, 8, "</title>") == 0) return true;
        }
        pos += 7;
    }
    return false;
}

bool is_transient(PortalError error) {
    return error == PortalError::timeout || error == PortalError::network || error == PortalError::dns;
}

bool should_fallback(PortalError error) {
    switch (error) {
        case PortalError::network: case PortalError::response: case PortalError::timeout:
        case PortalError::dns: case PortalError::httpStatus:
            return true;
        default:
            return false;
    }
}

PortalError after_submission(PortalError error) {
    switch (error) {
        case PortalError::network: case PortalError::timeout: case PortalError::dns: case PortalError::httpStatus:
        case PortalError::response: case PortalError::responseTooLarge: case PortalError::redirect:
            return PortalError::authenticationUncertain;
        default:
            return error;
    }
}

Outcome outcome_for(PortalError error, bool submitted) {
    switch (error) {
        case PortalError::none: return Outcome::success;
        case PortalError::invalidCredentials: return Outcome::invalidCredentials;
        case PortalError::challengeRefused: return Outcome::challengeRefused;
        case PortalError::rejected: return submitted ? Outcome::rejected : Outcome::challengeRefused;
        case PortalError::authenticationUncertain: return Outcome::uncertain;
        case PortalError::tls: case PortalError::forbiddenTarget: case PortalError::cryptography:
        case PortalError::protocolConflict:
            return Outcome::security;
        case PortalError::redirect: case PortalError::responseTooLarge:
            return submitted ? Outcome::uncertain : Outcome::security;
        case PortalError::network: case PortalError::timeout: case PortalError::dns: case PortalError::httpStatus:
        case PortalError::response:
            return submitted ? Outcome::uncertain : Outcome::unavailable;
    }
    return Outcome::unavailable;
}

}  // namespace szu::wire
