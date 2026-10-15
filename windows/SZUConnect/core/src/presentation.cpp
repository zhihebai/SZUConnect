#include "szu/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "../generated/shared_copy.hpp"

namespace szu {
namespace status {
namespace {
bool busy(const std::string& s) { return s == "checking" || s == "discovering" || s == "loggingIn" || s == "verifying"; }
bool auth_action(const std::optional<std::string>& a) {
    return a && (*a == "login" || *a == "resume" || *a == "trust" || *a == "forceLogin" || *a == "partialLogin");
}
bool can_login(const Input& in) { return in.has_draft && !in.read_only && in.mode != "observe"; }
void offline_actions(const Input& in, Presentation& p) {
    if (in.read_only || in.mode == "observe") { p.primary = "check"; p.secondary.reset(); return; }
    if (in.has_draft) { p.primary = "login"; p.secondary = "check"; return; }
    p.primary = "account";
    p.secondary = "check";
}
}  // namespace

Presentation present(const Input& in) {
    const std::string& s = in.status;
    Presentation p;
    p.tone = "neutral";
    p.headline = "status." + s + ".headline";
    p.detail = "status." + s + ".detail";
    if (s == "noNetwork") { p.tone = "warning"; p.secondary = "network"; }
    else if (s == "sleeping") { p.tone = "neutral"; }
    else if (busy(s)) { p.tone = "busy"; p.primary = "cancel"; }
    else if (s == "online") {
        p.tone = "success";
        p.primary = "check";
        p.detail = in.probe_mode == "complete" ? (!in.complete_partial && in.passed_count >= 2 ? "status.online.complete" : "status.online.partial") : "status.online.quick";
        if (in.website_only_issues) {
            p.tone = "warning";
            p.headline = "internet.websiteIssues.title";
            p.detail = "internet.websiteIssues.detail";
            p.secondary = "network";
            if (in.complete_partial && !in.recovery_evidence && can_login(in)) p.secondary = "forceLogin";
        }
        if (in.offer_forced_login && (!in.website_only_issues || can_login(in))) p.secondary = "forceLogin";
    } else if (s == "accepted") { p.tone = "info"; p.primary = "check"; }
    else if (s == "rejected") { p.tone = "danger"; p.primary = "account"; if (can_login(in)) p.secondary = "login"; }
    else if (s == "uncertain") { p.tone = "warning"; p.primary = "check"; }
    else if (s == "security") { p.tone = "danger"; p.primary = "network"; p.secondary = "check"; }
    else if (s == "unavailable") { p.tone = "warning"; offline_actions(in, p); }
    else if (s == "unconfirmed") { p.tone = "warning"; p.primary = "network"; p.secondary = "check"; }
    else if (s == "needsTrust") { p.tone = "warning"; p.primary = "trust"; p.secondary = "check"; }
    else if (s == "paused") {
        p.tone = "warning";
        p.primary = "resume";
        if (can_login(in)) p.secondary = "login";
        p.detail = in.hold ? "hold." + *in.hold + ".detail" : "status.paused.detail";
    } else if (s == "coolingDown") { p.tone = "info"; p.primary = "check"; }
    else if (s == "confirming") { p.tone = "info"; offline_actions(in, p); }
    else if (s == "needsAccount") { p.tone = "warning"; p.primary = "account"; p.secondary = "check"; }
    else if (s == "offline") {
        p.tone = "warning";
        if (in.diagnosis && *in.diagnosis == "portal") p.headline = "status.portal.headline";
        p.detail = in.diagnosis ? "diagnosis." + *in.diagnosis + ".detail" : "status.offline.detail";
        offline_actions(in, p);
    } else if (s == "cancelled") { p.tone = "neutral"; p.primary = "check"; }
    else { p.tone = "neutral"; p.primary = "check"; if (can_login(in)) p.secondary = "login"; }  // idle

    // Preserve busy/cancel and safety status. Only concrete recovery candidates
    // offer normal login; HTTP business responses use the explicit force confirmation.
    if ((in.recovery_evidence || (in.complete_partial && !in.website_only_issues)) &&
        !busy(s) && s != "noNetwork" && s != "sleeping" && can_login(in)) {
        p.primary = "partialLogin";
        p.secondary = "check";
    }
    if (in.read_only || in.mode == "observe") {
        if (auth_action(p.primary)) {
            if (s != "noNetwork" && s != "sleeping") p.primary = "check";
            else p.primary.reset();
        }
        if (auth_action(p.secondary) || p.secondary == p.primary) p.secondary.reset();
    }
    if (in.read_only) { p.chip = "auto.readOnly.chip"; p.chip_tone = "neutral"; }
    else if (in.mode != "recover") { p.chip = "auto." + in.mode + ".chip"; p.chip_tone = "neutral"; }
    else {
        p.chip = "auto." + in.auto_login + ".chip";
        const std::string& a = in.auto_login;
        p.chip_tone = a == "paused" ? "danger" : a == "coolingDown" ? "info" : a == "ready" ? "success" : "warning";
    }
    if (in.read_only) p.banner = Banner{"banner.readOnly", "info", std::nullopt};
    else if (in.mode == "recover" && in.auto_login == "paused") {
        const std::string hold = in.hold.value_or("credentials");
        const std::string tone = (hold == "security" || hold == "credentials") ? "danger" : hold == "uncertain" ? "warning" : "neutral";
        p.banner = Banner{"hold." + hold, tone, std::string("resume")};
    } else if (in.mode == "recover" && in.auto_login == "needsTrust") p.banner = Banner{"banner.needsTrust", "warning", std::string("trust")};
    else if (in.mode == "recover" && in.auto_login == "limited") p.banner = Banner{"banner.limited", "warning", std::string("guard")};
    else if (in.mode == "recover" && in.auto_login == "awaitingProof") p.banner = Banner{"banner.awaitingProof", "info", std::nullopt};
    if (!in.read_only && in.mode == "recover" && in.multiple_networks && in.auto_login == "awaitingProof") {
        p.banner = Banner{"banner.multipleNetworks", "warning", std::string("network")};
        if (s == "unconfirmed") {
            p.headline = "banner.multipleNetworks.title";
            p.detail = "banner.multipleNetworks.detail";
        }
    }
    return p;
}

Readiness readiness(const ReadinessInput& i) {
    Readiness r;
    if (i.mode != "recover" || i.read_only) { r.title = "ready.title.off"; r.tone = "neutral"; return r; }
    const std::string& a = i.auto_login;
    ReadinessItem account{"account", "ready.account.saved", "success"};
    if (!i.has_saved) account = {"account", "ready.account.missing", "warning"};
    else if (i.unsaved_changes) account = {"account", "ready.account.draft", "warning"};
    ReadinessItem network{"network", "ready.network.notNeeded", "neutral"};
    if (i.has_saved && !i.network_identified) network = {"network", "ready.network.identifying", "warning"};
    else if (a == "needsTrust") network = i.memory_conflict ? ReadinessItem{"network", "ready.network.conflict", "danger"} : ReadinessItem{"network", "ready.network.needsTrust", "warning"};
    else if (a == "awaitingProof") network = {"network", "ready.network.awaiting", "warning"};
    else if (i.session_trusted && !i.remembered) network = {"network", i.session_only_network ? "ready.network.trustedCombination" : "ready.network.trusted",
        i.campus_verification == "confirmed" ? "success" : "info"};
    else if (i.session_only_network) network = {"network", "ready.network.sessionAvailable", "info"};
    else if (i.remembered) network = {"network", "ready.network.remembered", i.campus_verification == "confirmed" ? "success" : "info"};
    const std::string hold = i.hold.value_or("");
    ReadinessItem protection{"protection", "ready.protection.ok", "success"};
    if (a == "paused") protection = {"protection", "ready.protection.paused", (hold == "credentials" || hold == "security") ? "danger" : "warning"};
    else if (a == "coolingDown") protection = {"protection", "ready.protection.cooldown", "info"};
    else if (a == "limited") protection = {"protection", "ready.protection.limited", "warning"};
    r.items = {account, network, protection};
    if (a == "paused") { r.title = "ready.title.paused"; r.tone = protection.tone; return r; }
    bool danger = false, warning = false;
    for (const auto& item : r.items) { danger |= item.tone == "danger"; warning |= item.tone == "warning"; }
    const bool campus_pending = i.campus_verification != "confirmed";
    r.title = (danger || warning) ? "ready.title.attention" : campus_pending ? "ready.title.campusPending" : "ready.title.ready";
    r.tone = danger ? "danger" : warning ? "warning" : campus_pending ? "info" : "success";
    return r;
}

ReadinessInput readiness_input_from(const EngineSnapshot& s) {
    ReadinessInput in;
    in.mode = std::string(to_string(s.mode));
    in.read_only = s.read_only;
    in.has_saved = s.has_saved;
    in.unsaved_changes = s.has_saved && s.unsaved_changes;
    in.auto_login = std::string(to_string(s.auto_login));
    if (s.pause_reason) in.hold = std::string(to_string(*s.pause_reason));
    in.session_trusted = s.trusted;
    in.remembered = s.remembered;
    in.memory_conflict = s.memory_level == MatchLevel::conflict;
    in.network_identified = s.network_available && !s.sleeping && (s.can_trust || s.trusted || s.remembered || s.authentication_route.has_value());
    const auto campus = campus_verification_key(s);
    in.campus_verification = std::string(campus.substr(campus.rfind('.') + 1));
    return in;
}

Input input_from(const EngineSnapshot& s) {
    Input in;
    in.status = std::string(to_string(s.status));
    if (s.diagnosis) in.diagnosis = std::string(to_string(*s.diagnosis));
    if (s.last_report) {
        in.probe_mode = std::string(to_string(s.last_report->mode));
        in.passed_count = s.last_report->passed_count();
        in.complete_partial = s.network_available && s.online && s.last_report->complete_partial();
        in.website_only_issues = s.network_available && s.online && s.last_report->website_only_issues();
        in.recovery_evidence = s.network_available && s.online && s.last_report->has_recovery_evidence();
    }
    in.mode = std::string(to_string(s.mode));
    in.read_only = s.read_only;
    in.has_draft = s.draft_complete;
    in.auto_login = std::string(to_string(s.auto_login));
    if (s.pause_reason) in.hold = std::string(to_string(*s.pause_reason));
    in.offer_forced_login = s.offer_forced_login;
    in.multiple_networks = s.multiple_networks;
    return in;
}
std::string_view campus_verification_key(const EngineSnapshot& s) {
    if (!s.network_available || s.sleeping || !s.last_discovery)
        return "env.campusVerification.notChecked";
    const auto& d = *s.last_discovery;
    const bool teaching = d.teaching == Discovery::confirmed;
    const bool confirmed = teaching && (s.area != CampusArea::dormitory || d.dormitory == Discovery::confirmed);
    if (confirmed)
        return "env.campusVerification.confirmed";
    return d.teaching == Discovery::notChecked && d.dormitory == Discovery::notChecked
        ? "env.campusVerification.notChecked" : "env.campusVerification.unconfirmed";
}
bool system_path_can_lead(const EngineSnapshot& s) {
    if (s.busy || s.sleeping || s.paused || !s.holds.empty() ||
        (s.cooldown_remaining && *s.cooldown_remaining > 0)) return false;
    switch (s.status) {
        case EngineStatus::idle: case EngineStatus::offline: case EngineStatus::online:
        case EngineStatus::noNetwork: case EngineStatus::unconfirmed: return true;
        // Public connectivity and campus-login prerequisites are separate facts.
        // Readiness still requests trust/account setup; protection guards above win.
        case EngineStatus::needsTrust: case EngineStatus::needsAccount: return true;
        default: return false;
    }
}

}  // namespace status

namespace records {

namespace {
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
}  // namespace

std::string iso8601(double unix_seconds) {
    std::int64_t t = static_cast<std::int64_t>(std::floor(unix_seconds));
    std::int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    std::int64_t secs = t - days * 86400;
    // civil_from_days (Howard Hinnant)
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    const int hour = static_cast<int>(secs / 3600), minute = static_cast<int>((secs / 60) % 60), second = static_cast<int>(secs % 60);
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02u-%02uT%02d:%02d:%02dZ", static_cast<long long>(y), m, d, hour, minute, second);
    return buffer;
}

std::optional<double> parse_iso8601(std::string_view text) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') return std::nullopt;
    const auto number = [text](std::size_t start, std::size_t count) {
        int value = 0;
        for (std::size_t i = start; i < start + count; ++i) {
            if (text[i] < '0' || text[i] > '9') return -1;
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    const int y = number(0, 4), mo = number(5, 2), d = number(8, 2),
              h = number(11, 2), mi = number(14, 2), s = number(17, 2);
    if (y < 0 || h < 0 || mi < 0 || s < 0) return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return std::nullopt;
    return static_cast<double>(days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + s);
}

bool EventLog::append(double now, std::string message, bool failure) {
    if (!items_.empty() && items_.back().message == message && now - items_.back().time < kDedupeSeconds) return false;
    items_.push_back(Event{now, std::move(message), failure});
    if (items_.size() > kLimit) items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(items_.size() - kLimit));
    return true;
}

json::Value EventLog::to_json() const {
    json::Value root = json::Value::object();
    root.set("schemaVersion", json::Value::number(1));
    json::Value list = json::Value::array();
    for (const Event& e : items_) {
        json::Value item = json::Value::object();
        item.set("date", json::Value::string(iso8601(e.time)));
        item.set("message", json::Value::string(e.message));
        if (e.failure) item.set("kind", json::Value::string("failure"));
        list.push(std::move(item));
    }
    root.set("events", std::move(list));
    return root;
}

std::optional<EventLog> EventLog::from_json(std::string_view text) {
    const auto root = json::parse(text);
    if (!root || !root->is_object()) return std::nullopt;
    const json::Value* list = root->find("events");
    if (!list || !list->is_array()) return std::nullopt;
    EventLog log;
    for (const json::Value& item : list->items()) {
        const auto date = item.string_at("date");
        const auto message = item.string_at("message");
        if (!date || !message) return std::nullopt;
        const auto time = parse_iso8601(*date);
        if (!time) return std::nullopt;
        log.items_.push_back(Event{*time, *message, item.string_at("kind").value_or("") == "failure"});
    }
    if (log.items_.size() > kLimit) log.items_.erase(log.items_.begin(), log.items_.end() - static_cast<std::ptrdiff_t>(kLimit));
    return log;
}

void HealthLog::append(const Sample& sample) {
    items_.push_back(sample);
    prune(sample.time);
}

void HealthLog::prune(double now) {
    // Reference = newest known time, so an out-of-order (clock moved back) sample never wipes newer ones.
    double reference = now;
    for (const Sample& s : items_) reference = std::max(reference, s.time);
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Sample& s) { return reference - s.time > kWindowSeconds; }),
                 items_.end());
    if (items_.size() > kLimit) items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(items_.size() - kLimit));
}

HealthSummary HealthLog::summary(double now) const {
    HealthSummary out;
    std::vector<double> latencies;
    for (const Sample& s : items_) {
        if (now - s.time > kWindowSeconds) continue;
        ++out.samples;
        if (s.online) ++out.online;
        if (s.latency_ms >= 0) latencies.push_back(s.latency_ms);
    }
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        const std::size_t n = latencies.size();
        out.median_latency_ms = n % 2 ? latencies[n / 2] : (latencies[n / 2 - 1] + latencies[n / 2]) / 2;
    }
    return out;
}

HealthRange HealthLog::range(double now, std::size_t ribbon) const {
    HealthRange out;
    std::vector<const Sample*> window;
    for (const Sample& s : items_)
        if (now - s.time <= kWindowSeconds) window.push_back(&s);
    out.count = static_cast<int>(window.size());
    if (window.empty()) return out;
    out.first = window.front()->time;
    out.last = window.back()->time;
    const std::size_t take = std::min(ribbon, window.size());
    out.ribbon_count = static_cast<int>(take);
    out.ribbon_first = window[window.size() - take]->time;
    out.ribbon_last = window.back()->time;
    return out;
}

json::Value HealthLog::to_json() const {
    json::Value root = json::Value::object();
    root.set("schemaVersion", json::Value::number(1));
    json::Value list = json::Value::array();
    for (const Sample& s : items_) {
        json::Value item = json::Value::object();
        item.set("date", json::Value::string(iso8601(s.time)));
        item.set("online", json::Value::boolean(s.online));
        item.set("mode", json::Value::string(s.mode));
        item.set("apple", json::Value::string(s.apple));
        item.set("microsoft", json::Value::string(s.microsoft));
        if (s.latency_ms >= 0) item.set("latencyMilliseconds", json::Value::number(std::round(s.latency_ms)));
        if (s.site_total >= 0 && s.site_total <= 12 && s.site_passed >= 0 && s.site_passed <= s.site_total) {
            item.set("websiteTotal", json::Value::number(s.site_total));
            item.set("websitePassed", json::Value::number(s.site_passed));
        }
        list.push(std::move(item));
    }
    root.set("samples", std::move(list));
    return root;
}

std::optional<HealthLog> HealthLog::from_json(std::string_view text) {
    const auto root = json::parse(text);
    if (!root || !root->is_object()) return std::nullopt;
    const json::Value* list = root->find("samples");
    if (!list || !list->is_array()) return std::nullopt;
    HealthLog log;
    for (const json::Value& item : list->items()) {
        const auto date = item.string_at("date");
        const auto online = item.bool_at("online");
        if (!date || !online) return std::nullopt;
        const auto time = parse_iso8601(*date);
        if (!time) return std::nullopt;
        Sample s;
        s.time = *time;
        s.online = *online;
        s.mode = item.string_at("mode").value_or("quick");
        s.apple = item.string_at("apple").value_or("notCompleted");
        s.microsoft = item.string_at("microsoft").value_or("notCompleted");
        s.latency_ms = item.number_at("latencyMilliseconds").value_or(-1);
        const double total = item.number_at("websiteTotal").value_or(-1), passed = item.number_at("websitePassed").value_or(-1);
        if (total >= 0 && total <= 12 && passed >= 0 && passed <= total && total == std::floor(total) && passed == std::floor(passed)) {
            s.site_total = static_cast<int>(total);
            s.site_passed = static_cast<int>(passed);
        }
        log.items_.push_back(std::move(s));
    }
    return log;
}

Sample sample_from(const ConnectivityReport& report, double now, int site_total, int site_passed) {
    Sample s;
    if (site_total >= 0 && site_total <= 12 && site_passed >= 0 && site_passed <= site_total) { s.site_total = site_total; s.site_passed = site_passed; }
    s.time = now;
    s.online = report.online();
    s.mode = std::string(to_string(report.mode));
    s.apple = std::string(to_string(report.apple.state));
    s.microsoft = std::string(to_string(report.microsoft.state));
    double best = -1;
    for (const EndpointResult* r : {&report.apple, &report.microsoft})
        if (r->succeeded() && (best < 0 || r->elapsed * 1000 < best)) best = r->elapsed * 1000;
    s.latency_ms = best;
    return s;
}

json::Value diagnostics_export(const ExportContext& c, const HealthLog& health, const EventLog& events) {
    json::Value root = json::Value::object();
    root.set("formatVersion", json::Value::number(1));
    root.set("exportedAt", json::Value::string(iso8601(c.now)));
    json::Value app = json::Value::object();
    app.set("version", json::Value::string(c.version));
    app.set("build", json::Value::string(c.build));
    app.set("platform", json::Value::string(c.platform));
    root.set("app", std::move(app));
    json::Value settings = json::Value::object();
    settings.set("monitoringMode", json::Value::string(c.mode));
    settings.set("area", json::Value::string(c.area));
    settings.set("probeMode", json::Value::string(c.probe_mode));
    root.set("settings", std::move(settings));
    const HealthSummary summary = health.summary(c.now);
    json::Value sum = json::Value::object();
    sum.set("samples", json::Value::number(summary.samples));
    sum.set("successPercent", json::Value::number(summary.success_percent()));
    if (summary.median_latency_ms) sum.set("medianLatencyMilliseconds", json::Value::number(std::round(*summary.median_latency_ms)));
    root.set("summary", std::move(sum));
    root.set("measurement", json::Value::string(std::string(copy::text("records.health.note"))));
    const json::Value health_json = health.to_json();
    const json::Value events_json = events.to_json();
    root.set("samples", *health_json.find("samples"));
    root.set("events", *events_json.find("events"));
    root.set("privacy", json::Value::string("No account, password, network name (SSID), website-check address or IP address is included."));
    return root;
}

std::string diagnostics_summary_text(const ExportContext& c, const HealthLog& health) {
    const HealthSummary s = health.summary(c.now);
    std::string text = std::string(copy::text("app.name")) + " " + c.version + " (" + c.build + ") · " + c.platform + "\n";
    text += copy::format("records.health.samples", {{"count", std::to_string(s.samples)}}) + " · " +
            copy::format("records.health.successRate", {{"percent", std::to_string(s.success_percent())}});
    if (s.median_latency_ms) text += " · " + copy::format("records.health.latency", {{"ms", std::to_string(static_cast<long>(std::lround(*s.median_latency_ms)))}});
    text += "\n" + std::string(copy::text("records.health.note"));
    return text;
}

}  // namespace records
}  // namespace szu
