#include "szu/internet.hpp"
#include "szu/environment.hpp"
#include "szu/json.hpp"
#include <algorithm>
#include <set>
#include <thread>

namespace szu {
bool WebsiteCheckResult::has_http_response() const {
    return status >= 200 && status < 600 && (state == "accessible" || state == "redirected" ||
        state == "authenticationRequired" || state == "forbidden" || state == "httpStatus");
}
std::size_t InternetCheckReport::passed_count() const {
    std::size_t count = static_cast<std::size_t>(base.passed_count());
    if (base.mode == ProbeMode::complete)
        for (const auto& site : websites) if (site.result.passed()) ++count;
    return count;
}
InternetVerdict InternetCheckReport::verdict() const {
    if (cancelled || (base.apple.state == EndpointState::cancelled && base.microsoft.state == EndpointState::cancelled)) return InternetVerdict::cancelled;
    if (base.mode == ProbeMode::quick) return base.online() ? InternetVerdict::basicPassed : InternetVerdict::unavailable;
    if (websites_deferred && base.all_passed()) return InternetVerdict::basicPassed;
    if (base.all_passed() && websites.size() == selected_count &&
        std::all_of(websites.begin(), websites.end(), [](const WebsiteCheck& site) { return site.result.passed(); }))
        return InternetVerdict::allPassed;
    return passed_count() ? InternetVerdict::partial : InternetVerdict::unavailable;
}
bool InternetCheckReport::website_only_issues() const {
    return !websites_deferred && base.all_passed() && verdict() == InternetVerdict::partial;
}
bool InternetCheckReport::has_recovery_evidence() const {
    return base.mode == ProbeMode::complete && verdict() == InternetVerdict::partial && base.online() &&
        (!base.all_passed() || std::any_of(websites.begin(), websites.end(), [](const WebsiteCheck& item) {
            return item.result.recovery_signal != WebsiteRecoverySignal::none;
        }));
}
bool InternetCheckReport::has_campus_recovery_evidence() const {
    return base.mode == ProbeMode::complete && verdict() == InternetVerdict::partial && base.online() &&
        (!base.all_passed() || std::any_of(websites.begin(), websites.end(), [](const WebsiteCheck& item) {
            return item.result.recovery_signal == WebsiteRecoverySignal::campusRedirect;
        }));
}

bool InternetCheckReport::transport_recovered() const {
    if (base.mode != ProbeMode::complete || !base.all_passed() || verdict() == InternetVerdict::cancelled ||
        websites.size() != selected_count) return false;
    std::set<std::string> ids;
    for (const auto& item : websites) {
        if (!ids.insert(item.target.id).second || !item.result.has_http_response() ||
            item.result.recovery_signal == WebsiteRecoverySignal::campusRedirect) return false;
    }
    return ids.size() == selected_count;
}
std::string_view to_string(InternetVerdict value) {
    switch (value) {
        case InternetVerdict::basicPassed: return "basicPassed";
        case InternetVerdict::allPassed: return "allPassed";
        case InternetVerdict::partial: return "partial";
        case InternetVerdict::unavailable: return "unavailable";
        case InternetVerdict::cancelled: return "cancelled";
    }
    return "unavailable";
}
namespace {
// Foundation CharacterSet.controlCharacters contains Unicode Cc and Cf. Decode validated UTF-8
// rather than treating continuation bytes as C1 controls, which would reject normal Chinese IDs.
bool valid_target_id(std::string_view id) {
    if (id.empty() || id.size() > 128 || !json::valid_utf8(id)) return false;
    bool visible = false;
    for (std::size_t i = 0; i < id.size();) {
        const auto first = static_cast<unsigned char>(id[i++]);
        std::uint32_t cp = first;
        int continuation = 0;
        if (first >= 0xF0) { cp = first & 7; continuation = 3; }
        else if (first >= 0xE0) { cp = first & 15; continuation = 2; }
        else if (first >= 0xC0) { cp = first & 31; continuation = 1; }
        while (continuation--) cp = (cp << 6) | (static_cast<unsigned char>(id[i++]) & 63);
        const bool control = cp <= 0x1F || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD ||
            (cp >= 0x600 && cp <= 0x605) || cp == 0x61C || cp == 0x6DD || cp == 0x70F ||
            (cp >= 0x890 && cp <= 0x891) || cp == 0x8E2 || cp == 0x180E ||
            (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
            (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) || cp == 0xFEFF ||
            (cp >= 0xFFF9 && cp <= 0xFFFB) || cp == 0x110BD || cp == 0x110CD ||
            (cp >= 0x13430 && cp <= 0x1345F) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
            (cp >= 0x1D173 && cp <= 0x1D17A) || cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F);
        if (control) return false;
        const bool whitespace = cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
            cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
        visible |= !whitespace;
    }
    return visible;
}
std::optional<std::string> target_url(std::string_view input) {
    const auto normalized = website::normalize(input);
    if (!normalized.ok()) return std::nullopt;
    std::string value = normalized.url;
    const auto authority_start = value.find("://") + 3;
    auto path_start = value.find('/', authority_start);
    if (path_start == std::string::npos) { value += '/'; path_start = value.size() - 1; }
    const auto port = value.find(':', authority_start);
    if (port < path_start && ((value.starts_with("https://") && value.substr(port, path_start - port) == ":443") ||
        (value.starts_with("http://") && value.substr(port, path_start - port) == ":80"))) value.erase(port, path_start - port);
    if (value.size() > 2048) return std::nullopt;
    return value;
}
}  // namespace

std::optional<std::vector<WebsiteTarget>> validated_website_targets(const std::vector<WebsiteTarget>& targets) {
    if (targets.size() > WebsiteTarget::maximum_count) return std::nullopt;
    std::vector<WebsiteTarget> clean;
    std::set<std::string> ids, urls;
    for (const auto& target : targets) {
        const auto normalized = target_url(target.url);
        if (!valid_target_id(target.id) || !normalized ||
            !ids.insert(target.id).second || !urls.insert(*normalized).second) return std::nullopt;
        clean.push_back({target.id, *normalized, target.enabled});
    }
    return clean;
}
std::optional<WebsitePreferences> WebsitePreferences::decode(std::string_view text) {
    const auto value = json::parse(text, {.max_depth = 8, .max_bytes = 32768});
    if (!value || !value->is_object() || value->number_at("schemaVersion") != 1) return std::nullopt;
    const auto* entries = value->find("targets");
    if (!entries || !entries->is_array() || entries->size() > WebsiteTarget::maximum_count) return std::nullopt;
    WebsitePreferences preferences;
    if (const auto* strategy = value->find("strategy"); strategy && !strategy->is_null()) {
        if (!strategy->is_string() || (strategy->as_string() != "system" && strategy->as_string() != "direct")) return std::nullopt;
        preferences.follow_system_proxy = strategy->as_string() == "system";
    }
    for (const auto& entry : entries->items()) {
        if (!entry.is_object()) return std::nullopt;
        const auto id = entry.string_at("id"), url = entry.string_at("url");
        const auto enabled = entry.bool_at("enabled");
        if (!id || !url || !enabled) return std::nullopt;
        preferences.targets.push_back({*id, *url, *enabled});
    }
    const auto clean = validated_website_targets(preferences.targets);
    if (!clean) return std::nullopt;
    preferences.targets = *clean;
    return preferences;
}
std::optional<std::string> WebsitePreferences::encode() const {
    const auto clean = validated_website_targets(targets);
    if (!clean) return std::nullopt;
    auto root = json::Value::object();
    root.set("schemaVersion", json::Value::number(1));
    root.set("strategy", json::Value::string(follow_system_proxy ? "system" : "direct"));
    auto entries = json::Value::array();
    for (const auto& target : *clean) {
        auto entry = json::Value::object();
        entry.set("id", json::Value::string(target.id)); entry.set("url", json::Value::string(target.url));
        entry.set("enabled", json::Value::boolean(target.enabled)); entries.push(std::move(entry));
    }
    root.set("targets", std::move(entries));
    auto result = json::serialize(root);
    if (result.size() > 32768) return std::nullopt;
    return result;
}
bool WebsitePreferencesState::load(ReadStatus status, std::string_view data) {
    if (status == ReadStatus::missing) { current_ = {}; preserve_existing_ = false; return true; }
    const auto decoded = status == ReadStatus::found ? WebsitePreferences::decode(data) : std::nullopt;
    if (!decoded) { preserve_existing_ = true; return false; }
    current_ = *decoded; preserve_existing_ = false; return true;
}
bool WebsitePreferencesState::save(const WebsitePreferences& next, const Writer& writer) {
    const auto encoded = next.encode();
    if (!encoded || !writer || !writer(*encoded, preserve_existing_)) return false;
    // The bytes accepted by the writer are the canonical runtime configuration too.
    current_ = *WebsitePreferences::decode(*encoded);
    preserve_existing_ = false;
    return true;
}
InternetCheckService::InternetCheckService(ConnectionService& base, WebsiteChecker checker)
    : base_(base), checker_(std::move(checker)) {}
bool InternetCheckService::configure(const std::vector<WebsiteTarget>& targets, bool follow_system_proxy) {
    const auto clean = validated_website_targets(targets);
    if (!clean) return false;
    CancelToken stale;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets_ = *clean;
        follow_system_proxy_ = follow_system_proxy;
        ++generation_;
        latest_.reset();
        stale = active_;
    }
    stale.cancel();  // callbacks may re-enter; never invoke them while holding mutex_
    return true;
}
void InternetCheckService::invalidate() {
    CancelToken stale;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        latest_.reset();
        stale = active_;
    }
    stale.cancel();
}
void InternetCheckService::invalidate_website_path() {
    CancelToken stale;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++website_path_generation_;
        // A proxy/TUN notification invalidates website results, not the owned
        // physical probe. Keep its endpoint reasons visible until the engine
        // confirms a physical path change and calls invalidate().
        if (latest_) {
            latest_->websites.clear();
            latest_->websites_deferred = true;
            latest_->base.websites_deferred = true;
            latest_->base.recovery_evidence = latest_->has_recovery_evidence();
            latest_->base.campus_recovery_evidence = latest_->has_campus_recovery_evidence();
            latest_->base.selected_transport_recovered = latest_->transport_recovered();
            if (latest_->base.mode == ProbeMode::complete)
                latest_->base.complete_result = latest_->base.online() ? CompleteCheckResult::partial : CompleteCheckResult::unavailable;
        }
        stale = active_website_;
    }
    stale.cancel(); // Never cancels the physical base/credential operation's parent token.
}

std::optional<InternetCheckReport> InternetCheckService::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}
ConnectivityReport InternetCheckService::probe(ProbeMode mode, const CancelToken& token) {
    return probe_with_scope(mode, token, false, false);
}

ConnectivityReport InternetCheckService::probe_with_scope(ProbeMode mode, const CancelToken& token,
                                                         bool prioritize_base, bool defer_websites) {
    InternetCheckReport report;
    report.base.mode = mode;
    CancelToken operation, website_operation;
    bool follow_system_proxy;
    std::uint64_t generation, website_path_generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = ++generation_;
        latest_.reset();
        active_ = operation;
        active_website_ = website_operation;
        website_path_generation = website_path_generation_;
        follow_system_proxy = follow_system_proxy_;
        report.follow_system_proxy = follow_system_proxy;
        for (const auto& target : targets_) if (target.enabled) {
            ++report.selected_count;
            if (mode == ProbeMode::complete) report.websites.push_back({target, {}});
        }
    }
    const auto registration = token.on_cancel([operation] { operation.cancel(); });
    const auto upstream_registration = operation.on_cancel([token] { token.cancel(); });
    const auto website_registration = operation.on_cancel([website_operation] { website_operation.cancel(); });
    bool base_acquired = false;
    if (mode == ProbeMode::complete && (prioritize_base || defer_websites)) {
        if (!operation.cancelled()) report.base = base_.probe(mode, operation);
        else report.base.apple.state = report.base.microsoft.state = EndpointState::cancelled;
        base_acquired = true;
        if (defer_websites || !report.base.all_passed()) {
            report.websites.clear();
            report.websites_deferred = true;
        }
    }
    std::vector<std::thread> workers;
    // Rows are fully allocated before workers start; each thread exclusively owns its own result.
    for (std::size_t i = 0; i < report.websites.size(); ++i) {
        workers.emplace_back([&, i] {
            if (website_operation.cancelled()) report.websites[i].result.state = "cancelled";
            else report.websites[i].result = checker_(report.websites[i].target.url, follow_system_proxy, website_operation);
        });
    }
    if (!base_acquired) {
        if (!operation.cancelled()) report.base = base_.probe(mode, operation);
        else report.base.apple.state = report.base.microsoft.state = EndpointState::cancelled;
    }
    for (auto& worker : workers) worker.join();
    token.remove(registration);
    operation.remove(upstream_registration);
    operation.remove(website_registration);
    // Final website-generation check, aggregate evidence and publication share one lock.
    // An invalidation cannot clear latest and then have this old website report restore it.
    std::lock_guard<std::mutex> publication(mutex_);
    if (website_path_generation != website_path_generation_) {
        report.websites.clear();
        report.websites_deferred = true;
    }
    report.cancelled = operation.cancelled();
    // Carry only the aggregate result. Built-in online facts remain unchanged; URLs and
    // website identities stay in this local presentation service.
    report.base.mode = mode;
    report.base.recovery_evidence = report.has_recovery_evidence();
    report.base.campus_recovery_evidence = report.has_campus_recovery_evidence();
    report.base.websites_deferred = report.websites_deferred;
    report.base.selected_transport_recovered = report.transport_recovered();
    if (mode == ProbeMode::complete) {
        switch (report.verdict()) {
            case InternetVerdict::allPassed: report.base.complete_result = CompleteCheckResult::allPassed; break;
            case InternetVerdict::basicPassed:
            case InternetVerdict::partial: report.base.complete_result = CompleteCheckResult::partial; break;
            case InternetVerdict::cancelled: report.base.complete_result = CompleteCheckResult::cancelled; break;
            default: report.base.complete_result = CompleteCheckResult::unavailable; break;
        }
    } else report.base.complete_result.reset();
    if (generation == generation_ && !report.cancelled && !token.cancelled()) latest_ = report;
    return report.base;
}
}  // namespace szu
