#include "szu/netlabels.hpp"
#include "szu/json.hpp"
#include <algorithm>
#include <cstdint>

namespace szu {
namespace {
bool valid_id(const std::string& id) {
    return id.size() >= 8 && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    });
}
bool valid_kind(const std::string& kind) { return kind == "wifi" || kind == "ethernet" || kind == "other"; }
// Display text excludes control characters, directional overrides and invisible format controls.
bool hidden(std::uint32_t cp) {
    return cp < 32 || (cp >= 127 && cp <= 159) || cp == 0x00AD || cp == 0x061C || cp == 0x06DD ||
        cp == 0x070F || cp == 0x08E2 || cp == 0x180E || (cp >= 0x0600 && cp <= 0x0605) ||
        (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
        (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) ||
        cp == 0x110BD || cp == 0x110CD || (cp >= 0x13430 && cp <= 0x13455) ||
        (cp >= 0x1BCA0 && cp <= 0x1BCA3) || (cp >= 0x1D173 && cp <= 0x1D17A) ||
        cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F);
}
std::uint32_t point(const std::string& s, std::size_t& position) {
    const auto lead = static_cast<unsigned char>(s[position++]);
    if (lead < 0x80) return lead;
    const unsigned following = lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
    std::uint32_t cp = lead & (following == 1 ? 0x1Fu : following == 2 ? 0x0Fu : 0x07u);
    for (unsigned i = 0; i < following; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[position++]) & 0x3Fu);
    return cp;
}
}  // namespace

std::string NetworkLabels::clean_name(const std::string& text) {
    if (!json::valid_utf8(text)) return {};
    std::string out;
    unsigned count = 0;
    for (std::size_t p = 0; p < text.size() && count < 80;) {
        const auto first = p;
        if (!hidden(point(text, p))) { out.append(text, first, p - first); ++count; }
    }
    const auto first = out.find_first_not_of(' ');
    return first == std::string::npos ? "" : out.substr(first, out.find_last_not_of(' ') - first + 1);
}
std::optional<std::string> NetworkLabels::valid_ssid(const std::optional<std::string>& value) {
    if (!value || value->empty() || value->size() > 32 || !json::valid_utf8(*value) || value->find_first_not_of(' ') == std::string::npos)
        return std::nullopt;
    for (std::size_t p = 0; p < value->size();) if (hidden(point(*value, p))) return std::nullopt;
    return value;
}
std::string NetworkLabels::fallback(const std::string& kind) {
    return kind == "wifi" ? "校园 Wi-Fi（名称待补全）" : kind == "ethernet" ? "校园有线网络" : "待命名的校园网络";
}
std::string NetworkLabels::display_name(const std::string& id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? fallback() : it->second.display_name;
}
bool NetworkLabels::update_automatic(const std::string& id, const NetworkLabel& observation) {
    if (!valid_id(id) || !valid_kind(observation.kind) || (!entries_.count(id) && entries_.size() >= 64)) return false;
    NetworkLabel next{clean_name(observation.display_name), observation.kind, valid_ssid(observation.ssid), false};
    if (next.display_name.empty()) next.display_name = fallback(next.kind);
    const auto it = entries_.find(id);
    if (it != entries_.end()) {
        const auto& old = it->second;
        const bool keep_wifi = old.ssid && !next.ssid && next.kind != "wifi";
        if (old.manual_name || keep_wifi || (old.ssid && !next.ssid && next.kind == "wifi")) next.display_name = old.display_name;
        if (keep_wifi) next.kind = old.kind;
        if (!next.ssid) next.ssid = old.ssid;
        next.manual_name = old.manual_name;
        if (next == old) return false;
    }
    entries_[id] = next;
    return true;
}
bool NetworkLabels::rename(const std::string& id, const std::string& name) {
    const auto cleaned = clean_name(name);
    if (!valid_id(id) || cleaned.empty() || (!entries_.count(id) && entries_.size() >= 64)) return false;
    auto next = entries_.count(id) ? entries_.at(id) : NetworkLabel{};
    next.display_name = cleaned;
    next.manual_name = true;
    if (entries_.count(id) && entries_.at(id) == next) return false;
    entries_[id] = next;
    return true;
}
bool NetworkLabels::retain(const std::set<std::string>& ids) {
    const auto old = entries_.size();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!ids.count(it->first)) it = entries_.erase(it); else ++it;
    }
    return entries_.size() != old;
}
std::string NetworkLabels::encode() const {
    auto root = json::Value::object(), labels = json::Value::object();
    root.set("schema", json::Value::number(1));
    for (const auto& [id, entry] : entries_) {
        auto item = json::Value::object();
        item.set("displayName", json::Value::string(entry.display_name));
        item.set("kind", json::Value::string(entry.kind));
        item.set("manualName", json::Value::boolean(entry.manual_name));
        if (entry.ssid) item.set("ssid", json::Value::string(*entry.ssid));
        labels.set(id, item);
    }
    root.set("labels", labels);
    return json::serialize(root);
}
std::optional<NetworkLabels> NetworkLabels::decode(const std::string& data) {
    const auto parsed = json::parse(data, {.max_depth = 8, .max_bytes = 65536});
    if (!parsed || parsed->number_at("schema") != 1) return std::nullopt;
    const auto* labels = parsed->find("labels");
    if (!labels || !labels->is_object() || labels->size() > 64) return std::nullopt;
    NetworkLabels result;
    for (const auto& id : labels->keys()) {
        const auto* item = labels->find(id);
        const auto name = item->string_at("displayName"), kind = item->string_at("kind");
        const auto manual = item->bool_at("manualName");
        if (!valid_id(id) || !name || clean_name(*name).empty() || !kind || !valid_kind(*kind) || !manual) return std::nullopt;
        const auto* ssid = item->find("ssid");
        if (ssid && !ssid->is_string() && !ssid->is_null()) return std::nullopt;
        result.entries_[id] = {clean_name(*name), *kind, valid_ssid(item->string_at("ssid")), *manual};
    }
    return result;
}
}  // namespace szu
