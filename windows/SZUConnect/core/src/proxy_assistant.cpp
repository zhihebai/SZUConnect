#include "szu/proxy_assistant.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "szu/json.hpp"

namespace szu::proxyassist {
namespace {
constexpr std::string_view campus_domain = "net.szu.edu.cn";
constexpr std::string_view dormitory_cidr = "172.30.255.42/32";

Patch fail(std::string_view input, std::string_view error) {
    return {std::string(input), {}, std::string(error), false};
}

// Retain the original tokens of unrelated settings, including large integer IDs.
// JSON Value intentionally stores numbers as double, so it is not used to rewrite a config.
struct Node {
    enum class Kind { object, array, string, primitive } kind = Kind::primitive;
    std::size_t start = 0, end = 0;
    std::string decoded;
    std::vector<std::string> keys;
    std::vector<Node> values;
    const Node* find(std::string_view key) const {
        if (kind != Kind::object) return nullptr;
        for (std::size_t i = 0; i < keys.size(); ++i) if (keys[i] == key) return &values[i];
        return nullptr;
    }
    bool is(Kind value) const { return kind == value; }
};

class Scanner {
public:
    explicit Scanner(std::string_view input) : input_(input) {}
    std::optional<Node> parse() {
        if (input_.empty() || input_.size() > kMaxConfigBytes || !json::valid_utf8(input_)) return std::nullopt;
        Node root;
        if (!value(root, 0)) return std::nullopt;
        skip();
        if (position_ != input_.size()) return std::nullopt;
        return root;
    }
private:
    std::string_view input_;
    std::size_t position_ = 0, nodes_ = 0;
    void skip() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }
    bool string(Node& node) {
        const auto begin = position_++;
        bool escaped = false, closed = false;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (escaped) { escaped = false; continue; }
            if (c == '\\') { escaped = true; continue; }
            if (c == '"') { closed = true; break; }
        }
        if (!closed) return false;
        const auto raw = input_.substr(begin, position_ - begin);
        const auto parsed = json::parse(raw, {1, kMaxConfigBytes});
        if (!parsed || !parsed->is_string()) return false;
        node.kind = Node::Kind::string;
        node.start = begin;
        node.end = position_;
        node.decoded = parsed->as_string();
        return true;
    }
    bool value(Node& node, std::size_t depth) {
        skip();
        if (depth > 48 || ++nodes_ > 32768 || position_ >= input_.size()) return false;
        node.start = position_;
        const char c = input_[position_];
        if (c == '"') return string(node);
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            node.kind = object ? Node::Kind::object : Node::Kind::array;
            const char close = object ? '}' : ']';
            ++position_;
            skip();
            std::set<std::string> keys;
            if (position_ < input_.size() && input_[position_] == close) {
                node.end = ++position_;
                return true;
            }
            for (;;) {
                if (object) {
                    Node key;
                    if (position_ >= input_.size() || input_[position_] != '"' || !string(key) ||
                        !keys.insert(key.decoded).second) return false;
                    node.keys.push_back(std::move(key.decoded));
                    skip();
                    if (position_ >= input_.size() || input_[position_++] != ':') return false;
                }
                Node child;
                if (!value(child, depth + 1)) return false;
                node.values.push_back(std::move(child));
                skip();
                if (position_ >= input_.size()) return false;
                if (input_[position_] == close) { node.end = ++position_; return true; }
                if (input_[position_++] != ',') return false;
                skip();
            }
        }
        const auto begin = position_;
        while (position_ < input_.size()) {
            const char next = input_[position_];
            if (next == ',' || next == '}' || next == ']' || next == ' ' || next == '\t' || next == '\r' || next == '\n') break;
            ++position_;
        }
        if (begin == position_) return false;
        const auto raw = input_.substr(begin, position_ - begin);
        const auto parsed = json::parse(raw, {1, kMaxConfigBytes});
        if (!parsed || parsed->is_string() || parsed->is_array() || parsed->is_object()) return false;
        node.kind = Node::Kind::primitive;
        node.decoded = std::string(raw);
        node.end = position_;
        return true;
    }
};

bool string_is(const Node* node, std::string_view text) {
    return node && node->is(Node::Kind::string) && node->decoded == text;
}
bool true_value(const Node* node) {
    return node && node->is(Node::Kind::primitive) && node->decoded == "true";
}
bool optional_bool(const Node* node) {
    return !node || (node->is(Node::Kind::primitive) && (node->decoded == "true" || node->decoded == "false"));
}
bool tag_value(const Node* node) {
    if (!node || !node->is(Node::Kind::string) || node->decoded.empty() || node->decoded.size() > 256) return false;
    return std::none_of(node->decoded.begin(), node->decoded.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool string_array(const Node* node) {
    return node && node->is(Node::Kind::array) && std::all_of(node->values.begin(), node->values.end(),
        [](const Node& value) { return value.is(Node::Kind::string); });
}
bool object_array(const Node* node) {
    return node && node->is(Node::Kind::array) && std::all_of(node->values.begin(), node->values.end(),
        [](const Node& value) { return value.is(Node::Kind::object); });
}
bool exact_item(const Node* node, std::string_view text) {
    return string_is(node, text) || (string_array(node) && node->values.size() == 1 && string_is(&node->values[0], text));
}

struct Edit { std::size_t start, length; std::string text; };
class Editor {
public:
    explicit Editor(std::string_view input) : input_(input) {}
    std::string raw(const Node& node) const { return std::string(input_.substr(node.start, node.end - node.start)); }
    void replace(const Node& node, std::string text) {
        edits_.push_back({node.start, node.end - node.start, std::move(text)});
    }
    void add(const Node& object, std::string key, std::string value) {
        auto& addition = additions_[object.end - 1];
        if (addition.empty() && !object.values.empty()) addition += ',';
        else if (!addition.empty()) addition += ',';
        addition += json::quote(key) + ":" + value;
    }
    std::string finish() {
        for (const auto& addition : additions_) edits_.push_back({addition.first, 0, addition.second});
        std::sort(edits_.begin(), edits_.end(), [](const Edit& a, const Edit& b) { return a.start > b.start; });
        std::string result(input_);
        for (const auto& edit : edits_) result.replace(edit.start, edit.length, edit.text);
        return result;
    }
private:
    std::string_view input_;
    std::vector<Edit> edits_;
    std::map<std::size_t, std::string> additions_;
};

std::string array_text(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& item : values) {
        if (result.size() > 1) result += ',';
        result += item;
    }
    return result + ']';
}

bool default_script(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    bool after_return = false;
    const auto line_break = [&](std::size_t position) -> std::size_t {
        if (text[position] == '\r' || text[position] == '\n') return 1;
        if (position + 2 < text.size() && static_cast<unsigned char>(text[position]) == 0xe2 &&
            static_cast<unsigned char>(text[position + 1]) == 0x80 &&
            (static_cast<unsigned char>(text[position + 2]) == 0xa8 || static_cast<unsigned char>(text[position + 2]) == 0xa9)) return 3;
        return 0;
    };
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const auto newline = line_break(i);
        if (newline) {
            if (after_return) return false;
            i += newline;
            continue;
        }
        if (c == ' ' || c == '\t') { ++i; continue; }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i += 2;
            while (i < text.size() && !line_break(i)) ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const auto close = text.find("*/", i + 2);
            if (close == std::string_view::npos) return false;
            if (after_return) for (auto j = i + 2; j < close; ++j) if (line_break(j)) return false;
            i = close + 2;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$') {
            const auto begin = i++;
            while (i < text.size()) {
                const unsigned char next = static_cast<unsigned char>(text[i]);
                if (!((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                    (next >= '0' && next <= '9') || next == '_' || next == '$')) break;
                ++i;
            }
            tokens.emplace_back(text.substr(begin, i - begin));
            after_return = tokens.back() == "return";
        } else if (c == '(' || c == ')' || c == '{' || c == '}' || c == ',' || c == ';') {
            tokens.emplace_back(1, static_cast<char>(c));
            after_return = false;
            ++i;
        } else return false;
        if (tokens.size() > 16) return false;
    }
    if (tokens.empty()) return true;
    if (!tokens.empty() && tokens.back() == ";") tokens.pop_back();
    if (tokens.empty()) return false;
    const std::vector<std::string> one{"function", "main", "(", "config", ")", "{", "return", "config", ";", "}"};
    const std::vector<std::string> two{"function", "main", "(", "config", ",", "profileName", ")", "{", "return", "config", ";", "}"};
    if (tokens == one || tokens == two) return true;
    const auto it = std::find(tokens.begin(), tokens.end(), ";");
    if (it == tokens.end()) tokens.insert(tokens.end() - 1, ";");
    return tokens == one || tokens == two;
}

bool owned_route_rule(const Node& node, std::string_view field, std::string_view target, std::string_view tag) {
    if (!node.is(Node::Kind::object) || (node.keys.size() != 2 && node.keys.size() != 3) ||
        !exact_item(node.find(field), target) || !string_is(node.find("outbound"), tag)) return false;
    if (node.keys.size() == 3 && !string_is(node.find("action"), "route")) return false;
    return std::all_of(node.keys.begin(), node.keys.end(), [field](const std::string& key) {
        return key == field || key == "outbound" || key == "action";
    });
}
bool owned_dns_rule(const Node& node, std::string_view tag) {
    if (!node.is(Node::Kind::object) || (node.keys.size() != 2 && node.keys.size() != 3) ||
        !exact_item(node.find("domain"), campus_domain) || !string_is(node.find("server"), tag)) return false;
    if (node.keys.size() == 3 && !string_is(node.find("action"), "route")) return false;
    return std::all_of(node.keys.begin(), node.keys.end(), [](const std::string& key) {
        return key == "domain" || key == "server" || key == "action";
    });
}
}  // namespace

std::string v2rayn_rules() {
    return R"JSON([
  {"remarks":"校园域名直连","enabled":true,"ruleType":1,"outboundTag":"direct","domain":["full:net.szu.edu.cn"]},
  {"remarks":"校园宿舍直连","enabled":true,"ruleType":1,"outboundTag":"direct","ip":["172.30.255.42/32"]}
])JSON";
}

std::string clash_verge_script() {
    return R"JS(// SZUConnect campus direct configuration v1
function main(config, profileName) {
  const error = (token) => { throw new Error("SZUConnect: " + token); };
  const object = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
  const list = (value) => value === undefined || (Array.isArray(value) && value.every((item) => typeof item === "string"));
  if (!object(config)) error("invalidConfig");
  if (config.mode !== undefined && config.mode !== "rule") error("unsupportedMode");
  if (!list(config.rules)) error("invalidConfig");
  const prefix = ["DOMAIN,net.szu.edu.cn,DIRECT", "IP-CIDR,172.30.255.42/32,DIRECT,no-resolve"];
  const result = { ...config, rules: [...prefix, ...(config.rules || []).filter((rule) => !prefix.includes(rule))] };
  if (config.dns !== undefined) {
    if (!object(config.dns)) error("invalidConfig");
    if (config.dns.enable !== undefined && typeof config.dns.enable !== "boolean") error("unsupportedDns");
    if (config.dns.enable !== false && config.dns["enhanced-mode"] === "fake-ip") {
      const mode = config.dns["fake-ip-filter-mode"] === undefined ? "blacklist" : config.dns["fake-ip-filter-mode"];
      if (mode !== "blacklist" && mode !== "rule") error("unsupportedDns");
      const previous = config.dns["fake-ip-filter"];
      if (!list(previous)) error("unsupportedDns");
      const exact = mode === "rule" ? "DOMAIN,net.szu.edu.cn,real-ip" : "net.szu.edu.cn";
      result.dns = { ...config.dns, "fake-ip-filter": [exact, ...(previous || []).filter((item) => item !== exact)] };
    }
  }
  if (config.tun !== undefined) {
    if (!object(config.tun)) error("invalidConfig");
    if ((config.tun.enable !== undefined && typeof config.tun.enable !== "boolean") ||
        (config.tun["auto-route"] !== undefined && typeof config.tun["auto-route"] !== "boolean")) error("unsupportedTun");
    if (config.tun.enable === true && config.tun["auto-route"] === true) {
      const previous = config.tun["route-exclude-address"];
      if (!list(previous)) error("unsupportedTun");
      const exact = "172.30.255.42/32";
      result.tun = { ...config.tun, "route-exclude-address": [...(previous || []).filter((item) => item !== exact), exact] };
    }
  }
  return result;
}
)JS";
}

Patch patch_clash_verge_script(std::string_view text) {
    if (text.size() > kMaxConfigBytes) return fail(text, "tooLarge");
    if (!json::valid_utf8(text)) return fail(text, "invalidConfig");
    const auto generated = clash_verge_script();
    if (text == generated) return {std::string(text), {}, {}, false};
    if (!default_script(text)) return fail(text, "unsupportedScript");
    return {generated, {"rules", "dns", "tun"}, {}, true};
}

Patch patch_sing_box(std::string_view text) {
    if (text.size() > kMaxConfigBytes) return fail(text, "tooLarge");
    const auto parsed = Scanner(text).parse();
    if (!parsed || !parsed->is(Node::Kind::object)) return fail(text, "invalidConfig");
    const auto& root = *parsed;
    const auto* outbounds = root.find("outbounds");
    if (!object_array(outbounds)) return fail(text, "unsupportedOutbound");
    std::set<std::string> outbound_tags;
    std::string direct_tag;
    for (const auto& outbound : outbounds->values) {
        const auto* type = outbound.find("type");
        const auto* tag = outbound.find("tag");
        if (!type || !type->is(Node::Kind::string) || (tag && !tag_value(tag))) return fail(text, "invalidConfig");
        if (tag && !outbound_tags.insert(tag->decoded).second) return fail(text, "unsupportedOutbound");
        if (!string_is(type, "direct")) continue;
        if (!direct_tag.empty() || !tag_value(tag)) return fail(text, "unsupportedOutbound");
        if ((outbound.find("detour") && !string_is(outbound.find("detour"), "")) ||
            outbound.find("override_address") || outbound.find("override_port")) return fail(text, "unsupportedOutbound");
        direct_tag = tag->decoded;
    }
    if (direct_tag.empty()) return fail(text, "unsupportedOutbound");

    const auto* route = root.find("route");
    if (route && !route->is(Node::Kind::object)) return fail(text, "invalidConfig");
    const auto* route_rules = route ? route->find("rules") : nullptr;
    if (route_rules && !object_array(route_rules)) return fail(text, "invalidConfig");
    bool modern = route_rules && std::any_of(route_rules->values.begin(), route_rules->values.end(),
        [](const Node& rule) { return rule.find("action") != nullptr; });

    const auto* dns = root.find("dns");
    if (dns && !dns->is(Node::Kind::object)) return fail(text, "unsupportedDns");
    const auto* dns_rules = dns ? dns->find("rules") : nullptr;
    const auto* servers = dns ? dns->find("servers") : nullptr;
    if (dns_rules && !object_array(dns_rules)) return fail(text, "unsupportedDns");
    if (servers && !object_array(servers)) return fail(text, "unsupportedDns");
    bool typed = false, legacy = false, fake_ip = false;
    std::set<std::string> dns_tags;
    std::vector<std::string> local_tags;
    if (servers) for (const auto& server : servers->values) {
        const auto* type = server.find("type");
        const auto* address = server.find("address");
        const auto* tag = server.find("tag");
        if ((type && !type->is(Node::Kind::string)) || (address && !address->is(Node::Kind::string)) ||
            (tag && !tag_value(tag))) return fail(text, "unsupportedDns");
        if (string_is(type, "")) type = nullptr;  // Empty type is the documented legacy default.
        if ((type && address) || (!type && !address)) return fail(text, "unsupportedDns");
        static const std::set<std::string> known_types{"local", "hosts", "tcp", "udp", "tls", "quic", "https", "h3",
            "dhcp", "mdns", "fakeip", "tailscale", "openconnect", "openvpn", "resolved"};
        if (type && !known_types.contains(type->decoded)) return fail(text, "unsupportedDns");
        if (tag && !dns_tags.insert(tag->decoded).second) return fail(text, "unsupportedDns");
        typed = typed || type;
        legacy = legacy || address;
        fake_ip = fake_ip || string_is(type, "fakeip") || string_is(address, "fakeip");
        const bool local = string_is(type, "local") || string_is(type, "dhcp") || string_is(address, "local") ||
            (address && address->decoded.rfind("dhcp://", 0) == 0);
        if (local) {
            const bool usable = tag_value(tag) && (!server.find("detour") || string_is(server.find("detour"), ""));
            local_tags.push_back(usable ? tag->decoded : "");
        }
    }
    if (typed && legacy) return fail(text, "unsupportedDns");
    modern = modern || typed;
    if (dns) {
        const auto* fake = dns->find("fakeip");
        if (fake) {
            if (!fake->is(Node::Kind::object) || !optional_bool(fake->find("enabled"))) return fail(text, "unsupportedDns");
            fake_ip = fake_ip || true_value(fake->find("enabled"));
        }
    }
    if (fake_ip && (local_tags.size() != 1 || local_tags[0].empty())) return fail(text, "unsupportedDns");

    const auto* inbounds = root.find("inbounds");
    if (inbounds && !object_array(inbounds)) return fail(text, "invalidConfig");
    std::vector<const Node*> tun_additions;
    if (inbounds) for (const auto& inbound : inbounds->values) {
        if (!string_is(inbound.find("type"), "tun")) continue;
        if (!optional_bool(inbound.find("auto_route")) || !optional_bool(inbound.find("strict_route"))) return fail(text, "unsupportedTun");
        if (!true_value(inbound.find("auto_route"))) continue;
        if (inbound.find("inet4_route_exclude_address") || inbound.find("inet6_route_exclude_address")) return fail(text, "unsupportedTun");
        const auto* exclusions = inbound.find("route_exclude_address");
        if ((exclusions && !string_array(exclusions)) || (!modern && !exclusions)) return fail(text, "unsupportedTun");
        const auto occurrences = exclusions ? std::count_if(exclusions->values.begin(), exclusions->values.end(),
            [](const Node& item) { return string_is(&item, dormitory_cidr); }) : 0;
        if (occurrences != 1) tun_additions.push_back(&inbound);
    }

    Editor editor(text);
    Patch result;
    const auto action = modern ? ",\"action\":\"route\"" : "";
    const auto domain_rule = "{\"domain\":[\"net.szu.edu.cn\"]" + std::string(action) + ",\"outbound\":" + json::quote(direct_tag) + "}";
    const auto ip_rule = "{\"ip_cidr\":[\"172.30.255.42/32\"]" + std::string(action) + ",\"outbound\":" + json::quote(direct_tag) + "}";
    const auto is_domain = [&](const Node& rule) { return owned_route_rule(rule, "domain", campus_domain, direct_tag); };
    const auto is_ip = [&](const Node& rule) { return owned_route_rule(rule, "ip_cidr", dormitory_cidr, direct_tag); };
    std::size_t owned = 0;
    if (route_rules) for (const auto& rule : route_rules->values) if (is_domain(rule) || is_ip(rule)) ++owned;
    const bool rules_ready = route_rules && route_rules->values.size() >= 2 && owned == 2 &&
        is_domain(route_rules->values[0]) && is_ip(route_rules->values[1]);
    if (!rules_ready) {
        std::vector<std::string> rules{domain_rule, ip_rule};
        if (route_rules) for (const auto& rule : route_rules->values) if (!is_domain(rule) && !is_ip(rule)) rules.push_back(editor.raw(rule));
        const auto replacement = array_text(rules);
        if (route_rules) editor.replace(*route_rules, replacement);
        else if (route) editor.add(*route, "rules", replacement);
        else editor.add(root, "route", "{\"rules\":" + replacement + "}");
        result.changes.push_back("rules");
    }
    if (fake_ip) {
        const auto& local = local_tags[0];
        const auto matches = [&](const Node& rule) { return owned_dns_rule(rule, local); };
        const auto count = dns_rules ? std::count_if(dns_rules->values.begin(), dns_rules->values.end(), matches) : 0;
        const bool ready = dns_rules && !dns_rules->values.empty() && count == 1 && matches(dns_rules->values[0]);
        if (!ready) {
            std::vector<std::string> rules{"{\"domain\":[\"net.szu.edu.cn\"]" + std::string(action) + ",\"server\":" + json::quote(local) + "}"};
            if (dns_rules) for (const auto& rule : dns_rules->values) if (!matches(rule)) rules.push_back(editor.raw(rule));
            const auto replacement = array_text(rules);
            if (dns_rules) editor.replace(*dns_rules, replacement);
            else editor.add(*dns, "rules", replacement);
            result.changes.push_back("dns");
        }
    }
    if (!tun_additions.empty()) {
        for (const auto* tun : tun_additions) {
            const auto* exclusions = tun->find("route_exclude_address");
            std::vector<std::string> values;
            if (exclusions) for (const auto& value : exclusions->values) if (!string_is(&value, dormitory_cidr)) values.push_back(editor.raw(value));
            values.push_back(json::quote(dormitory_cidr));
            if (exclusions) editor.replace(*exclusions, array_text(values));
            else editor.add(*tun, "route_exclude_address", array_text(values));
        }
        result.changes.push_back("tun");
    }
    result.text = editor.finish();
    if (result.text.size() > kMaxConfigBytes) return fail(text, "tooLarge");
    result.changed = result.text != text;
    return result;
}
}  // namespace szu::proxyassist
