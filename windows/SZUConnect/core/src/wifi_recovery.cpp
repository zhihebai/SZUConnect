#include "szu/wifi_recovery.hpp"
#include "szu/json.hpp"
#include <algorithm>

namespace szu::wifi {
namespace {
bool id_valid(const std::string& id) {
    return !id.empty() && id.size() <= 128 && id == NetworkLabels::clean_name(id);
}
bool digest_valid(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}
std::optional<std::string> temporary_open_profile(const std::string& ssid) {
    if (!NetworkLabels::valid_ssid(ssid)) return std::nullopt;
    std::string name, hex;
    constexpr char digits[] = "0123456789ABCDEF";
    for (const auto raw : ssid) {
        const auto byte = static_cast<unsigned char>(raw);
        hex += digits[byte >> 4]; hex += digits[byte & 15];
        switch (raw) {
            case '&': name += "&amp;"; break;
            case '<': name += "&lt;"; break;
            case '>': name += "&gt;"; break;
            case '\"': name += "&quot;"; break;
            case '\'': name += "&apos;"; break;
            default: name += raw; break;
        }
    }
    return "<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        "<name>" + name + "</name><SSIDConfig><SSID><hex>" + hex + "</hex></SSID></SSIDConfig>"
        "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security><authEncryption>"
        "<authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX>"
        "</authEncryption></security></MSM></WLANProfile>";
}

bool Network::valid() const { return id_valid(id) && NetworkLabels::valid_ssid(ssid).has_value(); }
std::optional<Preferences> Preferences::decode(const std::string& data) {
    const auto root = json::parse(data, {.max_depth = 8, .max_bytes = 16384});
    if (!root || root->number_at("schemaVersion") != 1 || !root->bool_at("autoJoinEnabled") ||
        !root->bool_at("powerOnWhenDisconnected")) return std::nullopt;
    Preferences value; value.auto_join = *root->bool_at("autoJoinEnabled"); value.power_on = *root->bool_at("powerOnWhenDisconnected");
    const auto* selections = root->find("selections");
    if (!selections || !selections->is_array() || selections->size() > 8) return std::nullopt;
    std::map<std::string, std::string> legacy;
    for (const auto& selection : selections->items()) {
        const auto id = selection.string_at("id"), digest = selection.string_at("ssidDigest");
        if (!id || !id_valid(*id) || !digest || !digest_valid(*digest) || legacy.count(*id)) return std::nullopt;
        legacy[*id] = *digest;
    }
    const auto* excluded = root->find("excludedNetworkIDs");
    if (!excluded || excluded->is_null()) value.legacy_selections = legacy;
    else {
        if (!excluded->is_array() || excluded->size() > 64) return std::nullopt;
        for (const auto& id : excluded->items()) {
            if (!id.is_string() || !id_valid(id.as_string()) || !value.excluded.insert(id.as_string()).second) return std::nullopt;
        }
    }
    if (const auto* manual = root->find("manualNetworkNames")) {
        if (!manual->is_object() || manual->size() > 64) return std::nullopt;
        for (const auto& id : manual->keys()) {
            const auto* ssid = manual->find(id);
            if (!id_valid(id) || !ssid->is_string() || !NetworkLabels::valid_ssid(ssid->as_string())) return std::nullopt;
            value.manual_names[id] = ssid->as_string();
        }
    }
    return value;
}
std::optional<std::string> Preferences::encode() const {
    auto root = json::Value::object(), selections = json::Value::array(), omitted = json::Value::array(), names = json::Value::object();
    root.set("schemaVersion", json::Value::number(1));
    root.set("autoJoinEnabled", json::Value::boolean(auto_join));
    root.set("powerOnWhenDisconnected", json::Value::boolean(power_on));
    if (legacy_selections) for (const auto& [id, digest] : *legacy_selections) {
        auto item = json::Value::object(); item.set("id", json::Value::string(id)); item.set("ssidDigest", json::Value::string(digest)); selections.push(item);
    }
    root.set("selections", selections);
    for (const auto& id : excluded) omitted.push(json::Value::string(id));
    if (!legacy_selections) root.set("excludedNetworkIDs", omitted);
    for (const auto& [id, ssid] : manual_names) names.set(id, json::Value::string(ssid));
    root.set("manualNetworkNames", names);
    const auto text = json::serialize(root);
    return decode(text) ? std::optional<std::string>(text) : std::nullopt;
}
bool Preferences::selected(const Network& network) const {
    if (!network.valid()) return false;
    if (legacy_selections) {
        const auto i = legacy_selections->find(network.id);
        return i != legacy_selections->end() && i->second == sha256_hex(network.ssid);
    }
    return !excluded.count(network.id);
}
bool Preferences::migrate(const std::vector<Network>& networks) {
    if (!legacy_selections || networks.empty()) return false;
    if (!legacy_selections->empty()) for (const auto& network : networks) if (!selected(network)) excluded.insert(network.id);
    legacy_selections.reset(); return true;
}
std::vector<Network> candidates(const std::vector<MemoryRecord>& records, const NetworkLabels& labels, const Preferences& preferences) {
    std::vector<Network> result;
    for (const auto& record : records) {
        const auto i = labels.entries().find(record.id);
        if (i == labels.entries().end() || i->second.kind != "wifi") continue;
        auto manual = preferences.manual_names.find(record.id);
        const auto ssid = manual != preferences.manual_names.end() ? std::optional<std::string>(manual->second) :
            (i == labels.entries().end() ? std::nullopt : i->second.ssid);
        if (!ssid) continue;
        Network network{record.id, *ssid, labels.display_name(record.id)};
        if (network.valid() && result.size() < 8) result.push_back(std::move(network));
    }
    return result;
}
std::string_view status_key(Result result) {
    switch (result) {
        case Result::cancelled: return "paused";
        case Result::alreadyConnected: return "connected";
        case Result::notFound: return "waiting";
        case Result::wifiOff: return "wifiOff";
        case Result::poweredOn: return "poweredOn";
        case Result::joined: return "joined";
        case Result::systemConnectionRequired: return "systemConnectionRequired";
        case Result::permissionRequired: return "permissionRequired";
        case Result::noSelection: return "noSelection";
        case Result::unavailable: return "unavailable";
        case Result::connectionUnclear: return "connectionUnclear";
        case Result::failed: return "failed";
    }
    return "failed";
}
Result recover(Radio& radio, const std::vector<Network>& networks, bool power, const CancelToken& token) {
    if (token.cancelled() || networks.size() > 8 || std::any_of(networks.begin(), networks.end(), [](const auto& n) { return !n.valid(); })) return Result::cancelled;
    auto gate = [&]() -> std::optional<Result> {
        if (token.cancelled()) return Result::cancelled;
        const auto state = radio.physical_state();
        if (state == PhysicalState::connected) return Result::alreadyConnected;
        if (state == PhysicalState::unknown) return Result::connectionUnclear;
        return token.cancelled() ? std::optional<Result>(Result::cancelled) : std::nullopt;
    };
    if (const auto denied = gate()) return *denied;
    const auto powered = radio.power_state();
    if (!powered) return Result::unavailable;
    if (!*powered) {
        if (!power) return Result::wifiOff;
        if (const auto denied = gate()) return *denied;
        const auto result = radio.power_on(token); return token.cancelled() ? Result::cancelled : result;
    }
    if (networks.empty()) return Result::noSelection;
    std::set<std::string> ssids;
    bool encrypted = false, failed = false;
    for (const auto& network : networks) {
        if (!ssids.insert(network.ssid).second) continue;
        if (const auto denied = gate()) return *denied;
        const auto scan = radio.scan(network.ssid, token);
        if (const auto denied = gate()) return *denied;
        if (scan.result == Result::permissionRequired || scan.result == Result::unavailable) return scan.result;
        failed |= scan.result == Result::failed;
        const bool protected_seen = std::any_of(scan.networks.begin(), scan.networks.end(), [&](const auto& candidate) {
            return candidate.ssid == network.ssid && (candidate.protected_network || candidate.saved_profile);
        });
        std::optional<ScanCandidate> chosen;
        for (const auto& candidate : scan.networks) if (candidate.ssid == network.ssid) {
            encrypted |= !candidate.open || candidate.protected_network || candidate.saved_profile;
            const bool profile = candidate.saved_profile && candidate.protected_network && !candidate.open;
            const bool open = candidate.open && !candidate.saved_profile && !protected_seen;
            if ((profile || open) && (!chosen || candidate.signal > chosen->signal)) chosen = candidate;
        }
        if (!chosen) continue;
        if (const auto denied = gate()) return *denied;
        const auto result = chosen->saved_profile ? radio.associate_profile(*chosen, token) : radio.associate_open(*chosen, token);
        if (token.cancelled()) return Result::cancelled;
        if (result == Result::joined || result == Result::alreadyConnected || result == Result::permissionRequired || result == Result::connectionUnclear) return result;
        if (result == Result::systemConnectionRequired) { encrypted = true; continue; }
        failed = true;
    }
    return failed ? Result::failed : encrypted ? Result::systemConnectionRequired : Result::notFound;
}
void Schedule::cancel() { token_.cancel(); active_ = false; result_.reset(); }
void Schedule::update(std::vector<Network> selected, bool enabled, bool power, bool allow, bool physical, bool sleeping) {
    const bool same_targets = selected.size() == selected_.size() && std::equal(selected.begin(),selected.end(),selected_.begin(),
        [](const auto& first,const auto& second) { return first.id == second.id && first.ssid == second.ssid; });
    if (same_targets && enabled == enabled_ && power == power_on_ && allow == allowed_ && physical == physical_ && sleeping == sleeping_) {
        selected_ = std::move(selected); return; // display-only rename cannot cancel a scan or restart retry backoff
    }
    cancel(); selected_ = std::move(selected); enabled_ = enabled; power_on_ = power; allowed_ = allow; physical_ = physical; sleeping_ = sleeping;
    next_ = 0; failures_ = 0;
}
bool Schedule::ready(double now) const {
    return !active_ && enabled_ && allowed_ && !physical_ && !sleeping_ && now >= next_ && (!selected_.empty() || power_on_);
}
std::optional<Schedule::Request> Schedule::begin(double now) {
    if (!ready(now)) return std::nullopt;
    token_ = CancelToken(); active_ = true;
    return Request{selected_, power_on_, token_};
}
void Schedule::finish(Result result, double now) {
    if (!active_ || token_.cancelled()) return;
    active_ = false; result_ = result;
    constexpr double transient_delays[]{2, 5, 10, 20, 30, 60, 120, 300};
    constexpr double protected_delays[]{15, 30, 60, 120, 300};
    const bool progress = result == Result::poweredOn || result == Result::joined || result == Result::alreadyConnected;
    const bool transient = result == Result::notFound || result == Result::failed;
    next_ = now + (progress ? 2 : transient ? transient_delays[std::min(failures_, 7u)] : protected_delays[std::min(failures_, 4u)]);
    failures_ = progress ? 0 : std::min(failures_ + 1, 7u);
}
std::string Schedule::status() const {
    if (!enabled_) return "disabled";
    if (!allowed_ || sleeping_) return "paused";
    if (physical_) return "connected";
    if (active_) return "searching";
    if (result_) return std::string(status_key(*result_));
    return selected_.empty() ? "noSelection" : "waiting";
}
} // namespace szu::wifi
