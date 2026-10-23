#include "szu/win/proxy_yaml.hpp"

#include <yaml-cpp/yaml.h>
#include <yaml-cpp/eventhandler.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace szu::win {
namespace {

constexpr std::string_view kDomain = "net.szu.edu.cn";
constexpr std::string_view kAddress = "172.30.255.42/32";
constexpr std::string_view kDomainRule = "DOMAIN,net.szu.edu.cn,DIRECT";
constexpr std::string_view kAddressRule = "IP-CIDR,172.30.255.42/32,DIRECT,no-resolve";
constexpr std::string_view kRealIpRule = "DOMAIN,net.szu.edu.cn,real-ip";
constexpr std::size_t kMaxDepth = 48;
constexpr std::size_t kMaxNodes = 65536;
constexpr std::size_t kMaxCollectionEntries = 16384;

struct InvalidYaml final : std::exception {};

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (!first) return false;
        if (first < 0x80) continue;
        std::size_t extra = 0;
        std::uint32_t value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            extra = 1; value = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            extra = 2; value = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            extra = 3; value = first & 7; minimum = 0x10000;
        } else return false;
        if (extra > text.size() - i) return false;
        for (std::size_t n = 0; n < extra; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool known_tag(const std::string& tag) {
    static const std::set<std::string> allowed = {
        "?", "!", "tag:yaml.org,2002:str", "tag:yaml.org,2002:null",
        "tag:yaml.org,2002:bool", "tag:yaml.org,2002:int", "tag:yaml.org,2002:float",
        "tag:yaml.org,2002:map", "tag:yaml.org,2002:seq", "tag:yaml.org,2002:binary",
        "tag:yaml.org,2002:timestamp", "tag:yaml.org,2002:set",
        "tag:yaml.org,2002:omap", "tag:yaml.org,2002:pairs"
    };
    return allowed.contains(tag);
}

// Validate the event stream before materializing a YAML graph. In particular,
// rejecting anchors and aliases here prevents cyclic/shared node expansion.
class BoundedEvents final : public YAML::EventHandler {
    struct Frame {
        bool mapping = false;
        bool key = true;
        std::size_t children = 0;
        std::set<std::string> keys;
    };
    std::vector<Frame> stack_;
    std::size_t nodes_ = 0, roots_ = 0, documents_ = 0;

    void node(bool scalar, const std::string* key = nullptr) {
        if (++nodes_ > kMaxNodes) throw InvalidYaml{};
        if (stack_.empty()) {
            if (++roots_ != 1 || scalar) throw InvalidYaml{};
            return;
        }
        auto& frame = stack_.back();
        if (++frame.children > kMaxCollectionEntries * (frame.mapping ? 2 : 1))
            throw InvalidYaml{};
        if (!frame.mapping) return;
        if (frame.key) {
            if (!scalar || !key || *key == "<<" || !frame.keys.insert(*key).second)
                throw InvalidYaml{};
        }
        frame.key = !frame.key;
    }

    void begin(bool mapping, const std::string& tag, YAML::anchor_t anchor) {
        if (anchor || !known_tag(tag) || stack_.size() >= kMaxDepth) throw InvalidYaml{};
        if (stack_.empty() && !mapping) throw InvalidYaml{};
        node(false);
        stack_.push_back(Frame{mapping});
    }

    void end(bool mapping) {
        if (stack_.empty() || stack_.back().mapping != mapping ||
            (mapping && !stack_.back().key)) throw InvalidYaml{};
        stack_.pop_back();
    }

public:
    void OnDocumentStart(const YAML::Mark&) override {
        if (++documents_ != 1) throw InvalidYaml{};
    }
    void OnDocumentEnd() override {
        if (!stack_.empty() || roots_ != 1) throw InvalidYaml{};
    }
    void OnNull(const YAML::Mark&, YAML::anchor_t anchor) override {
        if (anchor) throw InvalidYaml{};
        node(true);
    }
    void OnAlias(const YAML::Mark&, YAML::anchor_t) override { throw InvalidYaml{}; }
    void OnScalar(const YAML::Mark&, const std::string& tag, YAML::anchor_t anchor,
                  const std::string& value) override {
        if (anchor || !known_tag(tag)) throw InvalidYaml{};
        node(true, &value);
    }
    void OnSequenceStart(const YAML::Mark&, const std::string& tag, YAML::anchor_t anchor,
                         YAML::EmitterStyle::value) override { begin(false, tag, anchor); }
    void OnSequenceEnd() override { end(false); }
    void OnMapStart(const YAML::Mark&, const std::string& tag, YAML::anchor_t anchor,
                    YAML::EmitterStyle::value) override { begin(true, tag, anchor); }
    void OnMapEnd() override { end(true); }
    bool complete() const { return documents_ == 1 && roots_ == 1 && stack_.empty(); }
};

YAML::Node get(const YAML::Node& node, const char* key) {
    if (!node.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
    return node[key];
}

std::string_view trim(std::string_view value) {
    constexpr std::string_view space = " \t\r\n";
    const auto first = value.find_first_not_of(space);
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(space) - first + 1);
}

std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

std::vector<std::string_view> fields(std::string_view value) {
    std::vector<std::string_view> result;
    for (;;) {
        const auto next = value.find(',');
        result.push_back(trim(value.substr(0, next)));
        if (next == std::string_view::npos) return result;
        value.remove_prefix(next + 1);
    }
}

bool campus_route(std::string_view value) {
    const auto parts = fields(value);
    if (parts.size() == 3 && parts[0] == "DOMAIN" && lower(parts[1]) == kDomain &&
        parts[2] == "DIRECT") return true;
    return parts.size() == 4 && parts[0] == "IP-CIDR" && parts[1] == kAddress &&
        parts[2] == "DIRECT" && parts[3] == "no-resolve";
}

bool real_ip_rule(std::string_view value) {
    const auto parts = fields(value);
    return parts.size() == 3 && parts[0] == "DOMAIN" && lower(parts[1]) == kDomain &&
        parts[2] == "real-ip";
}

bool strings(const YAML::Node& sequence, std::vector<std::string>& values) {
    if (!sequence.IsSequence()) return false;
    values.reserve(sequence.size());
    for (const auto& item : sequence) {
        if (!item.IsScalar()) return false;
        values.push_back(item.Scalar());
    }
    return true;
}

YAML::Node sequence(const std::vector<std::string>& values) {
    YAML::Node result(YAML::NodeType::Sequence);
    for (const auto& value : values) result.push_back(value);
    return result;
}

proxyassist::Patch failure(const char* error) {
    proxyassist::Patch result;
    result.error = error;
    return result;
}

bool boolean(const YAML::Node& node, bool fallback) {
    return !node.IsDefined() || node.IsNull() ? fallback : node.as<bool>();
}

void emit(YAML::Emitter& output, const YAML::Node& node) {
    const auto& tag = node.Tag();
    if (!tag.empty() && tag != "?" && tag != "!") output << YAML::VerbatimTag(tag);
    if (node.IsNull()) {
        output << YAML::Null;
    } else if (node.IsScalar()) {
        // yaml-cpp's default Node emitter discards the non-specific string tag.
        // Keep quoted boolean/numeric-looking strings as strings on round-trip.
        if (tag == "!") output << YAML::DoubleQuoted;
        output << node.Scalar();
    } else if (node.IsSequence()) {
        output << YAML::BeginSeq;
        for (const auto& item : node) emit(output, item);
        output << YAML::EndSeq;
    } else if (node.IsMap()) {
        output << YAML::BeginMap;
        for (const auto& item : node) {
            output << YAML::Key;
            emit(output, item.first);
            output << YAML::Value;
            emit(output, item.second);
        }
        output << YAML::EndMap;
    } else throw InvalidYaml{};
}

}  // namespace

proxyassist::Patch patch_mihomo_yaml(std::string_view text) {
    if (text.size() > proxyassist::kMaxConfigBytes) return failure("tooLarge");
    if (text.empty() || !valid_utf8(text)) return failure("invalidConfig");
    try {
        BoundedEvents events;
        std::istringstream input{std::string(text)};
        YAML::Parser parser(input);
        while (parser.HandleNextDocument(events)) {}
        if (!events.complete()) return failure("invalidConfig");
        auto document = YAML::Load(std::string(text));
        if (!document.IsMap()) return failure("invalidConfig");

        const auto mode = get(document, "mode");
        if (mode.IsDefined() && (!mode.IsScalar() || lower(mode.Scalar()) != "rule"))
            return failure("unsupportedMode");

        proxyassist::Patch result;
        std::vector<std::string> old_rules;
        const auto old_rule_node = get(document, "rules");
        if (old_rule_node.IsDefined() && !strings(old_rule_node, old_rules))
            return failure("invalidConfig");
        std::vector<std::string> new_rules{std::string(kDomainRule), std::string(kAddressRule)};
        for (const auto& rule : old_rules) if (!campus_route(rule)) new_rules.push_back(rule);
        if (new_rules != old_rules) {
            document["rules"] = sequence(new_rules);
            result.changes.emplace_back("rules");
        }

        auto dns = get(document, "dns");
        if (dns.IsDefined() && !dns.IsNull()) {
            if (!dns.IsMap()) return failure("unsupportedDns");
            bool dns_enabled = true;
            try { dns_enabled = boolean(get(dns, "enable"), true); }
            catch (...) { return failure("unsupportedDns"); }
            if (dns_enabled) {
                const auto enhanced = get(dns, "enhanced-mode");
                if (enhanced.IsDefined() && !enhanced.IsScalar()) return failure("unsupportedDns");
                if (enhanced.IsScalar() && lower(enhanced.Scalar()) == "fake-ip") {
                    const auto filter_mode_node = get(dns, "fake-ip-filter-mode");
                    if (filter_mode_node.IsDefined() && !filter_mode_node.IsScalar())
                        return failure("unsupportedDns");
                    const auto filter_mode = filter_mode_node.IsDefined() ?
                        lower(filter_mode_node.Scalar()) : "blacklist";
                    if (filter_mode != "blacklist" && filter_mode != "rule")
                        return failure("unsupportedDns");
                    std::vector<std::string> old_filter;
                    const auto filter = get(dns, "fake-ip-filter");
                    if (filter.IsDefined() && !strings(filter, old_filter)) return failure("unsupportedDns");
                    auto new_filter = old_filter;
                    if (filter_mode == "blacklist") {
                        if (std::none_of(new_filter.begin(), new_filter.end(), [](const auto& item) {
                                return lower(trim(item)) == kDomain;
                            })) new_filter.emplace_back(kDomain);
                    } else {
                        new_filter.clear();
                        new_filter.emplace_back(kRealIpRule);
                        for (const auto& item : old_filter) if (!real_ip_rule(item)) new_filter.push_back(item);
                    }
                    if (new_filter != old_filter) {
                        dns["fake-ip-filter"] = sequence(new_filter);
                        result.changes.emplace_back("dns");
                    }
                }
            }
        }

        auto tun = get(document, "tun");
        if (tun.IsDefined() && !tun.IsNull()) {
            if (!tun.IsMap()) return failure("invalidConfig");
            if (boolean(get(tun, "enable"), false) && boolean(get(tun, "auto-route"), false)) {
                std::vector<std::string> addresses;
                const auto excluded = get(tun, "route-exclude-address");
                if (excluded.IsDefined() && !strings(excluded, addresses)) return failure("invalidConfig");
                if (std::find(addresses.begin(), addresses.end(), kAddress) == addresses.end()) {
                    addresses.emplace_back(kAddress);
                    tun["route-exclude-address"] = sequence(addresses);
                    result.changes.emplace_back("tun");
                }
            }
        }

        result.changed = !result.changes.empty();
        if (!result.changed) {
            result.text.assign(text);
            return result;
        }
        YAML::Emitter output;
        emit(output, document);
        if (!output.good() || output.size() >= proxyassist::kMaxConfigBytes)
            return failure("invalidConfig");
        result.text.assign(output.c_str(), output.size());
        result.text.push_back('\n');
        return result;
    } catch (...) {
        return failure("invalidConfig");
    }
}

}  // namespace szu::win
