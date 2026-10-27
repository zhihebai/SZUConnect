#include "szu/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <initializer_list>

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
    if (s == "online" && in.bound_website_only_issues) {
        p.primary = "check";
        p.secondary = "network";
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
    if (s == "unconfirmed" && in.needs_selection && !in.read_only && !in.hold && in.auto_login != "paused") {
        p.headline = "campus.entry.pendingTitle";
        p.detail = "campus.entry.bothDetected";
        p.primary = "check";
        p.secondary = "area";
        p.chip = "campus.entry.pendingTitle";
        p.chip_tone = "warning";
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
    in.remembered = s.recovery_setup_has_network;
    in.memory_conflict = s.memory_level == MatchLevel::conflict;
    in.network_identified = s.network_available && !s.sleeping && (s.can_trust || s.trusted || s.remembered || s.authentication_route.has_value());
    const auto campus = campus_verification_key(s);
    in.campus_verification = std::string(campus.substr(campus.rfind('.') + 1));
    return in;
}

Readiness recovery_readiness(const EngineSnapshot& s, bool service_verification_failed) {
    const auto input = readiness_input_from(s);
    auto result = readiness(input);
    const bool protected_item = std::any_of(result.items.begin(), result.items.end(), [](const ReadinessItem& item) {
        return item.kind != "network" && (item.tone == "warning" || item.tone == "danger");
    });
    if (s.mode != MonitoringMode::recover || s.read_only || !s.recovery_setup_has_network || !input.network_identified ||
        s.sleeping || s.paused || !s.holds.empty() || protected_item ||
        (s.cooldown_remaining && *s.cooldown_remaining > 0) ||
        (s.auto_login != AutoLoginState::ready && s.auto_login != AutoLoginState::awaitingProof) ||
        (s.auto_login == AutoLoginState::awaitingProof && input.campus_verification == "confirmed" &&
            !s.verifying_memory && !s.verifying_campus)) return result;
    switch (s.status) {
        case EngineStatus::security: case EngineStatus::rejected: case EngineStatus::uncertain:
        case EngineStatus::paused: case EngineStatus::coolingDown: case EngineStatus::sleeping:
        case EngineStatus::loggingIn: case EngineStatus::verifying: case EngineStatus::cancelled:
            return result;
        default: break;
    }
    const bool checking = input.campus_verification == "checking";
    if (checking) {
        result.title = "ready.title.checking";
        result.tone = "busy";
    } else if (input.campus_verification == "unconfirmed" || s.automatic_campus_verification_failed ||
        (input.campus_verification == "notChecked" && service_verification_failed)) {
        result.title = "ready.title.verificationFailed";
        result.tone = "warning";
    } else if (s.online && input.campus_verification == "notChecked") {
        result.title = "ready.title.monitoring";
        result.tone = "info";
    }
    for (auto& item : result.items) {
        if (item.kind != "network") continue;
        item.key = checking ? "ready.network.knownChecking" : "ready.network.remembered";
        item.tone = input.campus_verification == "confirmed" ? "success" : "info";
    }
    return result;
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
        in.bound_website_only_issues = s.authentication_route.has_value() && in.website_only_issues &&
            !s.last_report->campus_recoverable_partial();
        if (in.bound_website_only_issues) in.recovery_evidence = false;
    }
    in.mode = std::string(to_string(s.mode));
    in.read_only = s.read_only;
    in.has_draft = s.draft_complete;
    in.auto_login = std::string(to_string(s.auto_login));
    if (s.pause_reason) in.hold = std::string(to_string(*s.pause_reason));
    in.offer_forced_login = s.offer_forced_login;
    in.multiple_networks = s.multiple_networks;
    in.needs_selection = s.needs_selection;
    return in;
}
std::string_view campus_verification_key(const EngineSnapshot& s) {
    if (s.network_available && !s.sleeping && (s.verifying_memory || s.verifying_campus ||
        (s.busy && s.status == EngineStatus::discovering))) return "env.campusVerification.checking";
    if (!s.network_available || s.sleeping || !s.last_discovery)
        return "env.campusVerification.notChecked";
    if (s.required_campus_verification == Discovery::confirmed) return "env.campusVerification.confirmed";
    return s.required_campus_verification == Discovery::notChecked
        ? "env.campusVerification.notChecked" : "env.campusVerification.unconfirmed";
}
std::string_view campus_verification_hint_key(const EngineSnapshot& s, bool service_verification_failed) {
    const auto state = campus_verification_key(s);
    if (state == "env.campusVerification.checking") return "campus.verify.checking";
    if (state == "env.campusVerification.confirmed") return "campus.verify.confirmedHint";
    if (state == "env.campusVerification.unconfirmed" && s.last_discovery) {
        switch (s.last_discovery->issue) {
            case CampusVerificationIssue::dnsTimeout: return "campus.verify.dnsTimeoutHint";
            case CampusVerificationIssue::dns: return "campus.verify.dnsHint";
            case CampusVerificationIssue::dnsTls: return "campus.verify.dnsTlsHint";
            case CampusVerificationIssue::tls: return "campus.verify.tlsHint";
            case CampusVerificationIssue::connection: return "campus.verify.connectionHint";
            case CampusVerificationIssue::http: return "campus.verify.httpHint";
            case CampusVerificationIssue::page: return "campus.verify.pageHint";
            case CampusVerificationIssue::binding: return "campus.verify.bindingHint";
            case CampusVerificationIssue::none: break;
        }
    }
    if (s.automatic_campus_verification_pending) return "campus.verify.autoFailed";
    if (state == "env.campusVerification.unconfirmed" || service_verification_failed) return "campus.verify.failedHint";
    if (s.mode == MonitoringMode::recover && s.remembered && s.recovery_setup_has_network && s.remember_blocker.empty() &&
        readiness_input_from(s).network_identified && !s.read_only) return "campus.verify.autoPendingHint";
    return s.online ? "campus.verify.onlineHint" : "campus.verify.pendingHint";
}
bool system_path_can_lead(const EngineSnapshot& s) {
    if (s.busy || s.sleeping || s.paused || s.needs_selection || !s.holds.empty() ||
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

namespace {
std::optional<FailureContext> safe_failure_context(const std::optional<FailureContext>&);
json::Value failure_context_json(const FailureContext&, bool timestamps = true);
std::optional<FailureContext> failure_context_from_json(const json::Value*);
}

bool EventLog::append(double now, std::string message, bool failure, const std::optional<FailureContext>& context) {
    const auto safe = safe_failure_context(context);
    const auto signature = [](const std::optional<FailureContext>& c) { return c ? json::serialize(failure_context_json(*c, false)) : std::string(); };
    if (!items_.empty() && items_.back().message == message && now - items_.back().time < kDedupeSeconds &&
        signature(items_.back().context) == signature(safe)) return false;
    items_.push_back(Event{now, std::move(message), failure, safe});
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
        if (e.context) item.set("context", failure_context_json(*e.context));
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
        log.items_.push_back(Event{*time, *message, item.string_at("kind").value_or("") == "failure", failure_context_from_json(item.find("context"))});
    }
    if (log.items_.size() > kLimit) log.items_.erase(log.items_.begin(), log.items_.end() - static_cast<std::ptrdiff_t>(kLimit));
    return log;
}

namespace {
bool sample_token(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}
bool sample_time(double value) { return std::isfinite(value) && value >= -62135596800.0 && value <= 253402300799.0; }
std::optional<double> sample_duration(std::optional<double> value) {
    return value && std::isfinite(*value) && *value >= 0 && *value <= 120000 ? value : std::nullopt;
}
std::string sample_endpoint(std::string_view value) {
    return sample_token(value, {"success", "timeout", "dns", "tls", "redirect", "unexpectedResponse", "network", "cancelled", "notCompleted"})
        ? std::string(value) : "notCompleted";
}
DiagnosticEnvironment sample_environment(const DiagnosticEnvironment& input) {
    DiagnosticEnvironment safe;
    safe.refreshing = input.refreshing;
    if (!safe.refreshing) {
        if (sample_token(input.connection, {"wifi", "ethernet", "multiple", "tunnelHidden", "none", "unknown"})) safe.connection = input.connection;
        if (sample_token(input.system_proxy, {"on", "off", "unknown"})) safe.system_proxy = input.system_proxy;
        if (sample_token(input.tun, {"detected", "notDetected", "unknown"})) safe.tun = input.tun;
        if (sample_token(input.physical_egress, {"normal", "forwarding", "weakHostSend", "forwardingAndWeakHostSend", "unknown"}))
            safe.physical_egress = input.physical_egress;
    }
    return safe;
}
std::optional<SampleContext> sample_context(const std::optional<SampleContext>& input) {
    if (!input || !sample_time(input->started_at) || input->phase != "connectivityCheck" ||
        !sample_token(input->operation, {"manual", "automatic"}) ||
        !sample_token(input->monitoring_mode, {"manual", "observe", "recover"}) ||
        !sample_token(input->area, {"automatic", "teaching", "dormitory"})) return std::nullopt;
    SampleContext safe = *input;
    safe.environment = sample_environment(input->environment);
    return safe;
}
Sample safe_sample(Sample input) {
    input.mode = input.mode == "complete" ? "complete" : "quick";
    input.apple = sample_endpoint(input.apple); input.microsoft = sample_endpoint(input.microsoft);
    if (!std::isfinite(input.latency_ms) || input.latency_ms < 0 || input.latency_ms > 120000) input.latency_ms = -1;
    input.apple_ms = sample_duration(input.apple_ms); input.microsoft_ms = sample_duration(input.microsoft_ms);
    if (input.apple == "notCompleted") input.apple_ms.reset();
    if (input.microsoft == "notCompleted") input.microsoft_ms.reset();
    if (!sample_token(input.diagnosis, {"online", "portal", "response", "security", "dns", "timeout", "unreachable", "incomplete"})) input.diagnosis.clear();
    if (!sample_token(input.complete_result, {"allPassed", "partial", "unavailable", "cancelled"})) input.complete_result.clear();
    input.context = sample_context(input.context);
    if (!(input.site_total >= 0 && input.site_total <= 12 && input.site_passed >= 0 && input.site_passed <= input.site_total))
        input.site_total = input.site_passed = -1;
    return input;
}
json::Value context_json(const SampleContext& c) {
    auto value = json::Value::object();
    value.set("startedAt", json::Value::string(iso8601(c.started_at)));
    value.set("phase", json::Value::string(c.phase));
    value.set("operation", json::Value::string(c.operation));
    value.set("monitoringMode", json::Value::string(c.monitoring_mode));
    value.set("area", json::Value::string(c.area));
    auto environment = json::Value::object();
    environment.set("connection", json::Value::string(c.environment.connection));
    environment.set("systemProxy", json::Value::string(c.environment.system_proxy));
    environment.set("tun", json::Value::string(c.environment.tun));
    environment.set("physicalEgress", json::Value::string(c.environment.physical_egress));
    environment.set("refreshing", json::Value::boolean(c.environment.refreshing));
    value.set("environment", std::move(environment));
    return value;
}
std::optional<SampleContext> context_from_json(const json::Value* value) {
    if (!value || !value->is_object()) return std::nullopt;
    const auto started = value->string_at("startedAt");
    const auto time = started ? parse_iso8601(*started) : std::nullopt;
    const auto* environment = value->find("environment");
    if (!time || !environment || !environment->is_object()) return std::nullopt;
    SampleContext context;
    context.started_at = *time;
    context.phase = value->string_at("phase").value_or("");
    context.operation = value->string_at("operation").value_or("");
    context.monitoring_mode = value->string_at("monitoringMode").value_or("");
    context.area = value->string_at("area").value_or("");
    context.environment.connection = environment->string_at("connection").value_or("unknown");
    context.environment.system_proxy = environment->string_at("systemProxy").value_or("unknown");
    context.environment.tun = environment->string_at("tun").value_or("unknown");
    context.environment.physical_egress = environment->string_at("physicalEgress").value_or("unknown");
    context.environment.refreshing = environment->bool_at("refreshing").value_or(false);
    return sample_context(context);
}

std::string_view failure_issue_token(CampusVerificationIssue issue) {
    switch (issue) {
        case CampusVerificationIssue::dnsTimeout: return "dnsTimeout";
        case CampusVerificationIssue::dns: return "dns";
        case CampusVerificationIssue::dnsTls: return "dnsTls";
        case CampusVerificationIssue::tls: return "tls";
        case CampusVerificationIssue::connection: return "connection";
        case CampusVerificationIssue::http: return "http";
        case CampusVerificationIssue::page: return "page";
        case CampusVerificationIssue::binding: return "binding";
        case CampusVerificationIssue::none: return "none";
    }
    return "none";
}
std::optional<FailureContext> safe_failure_context(const std::optional<FailureContext>& input) {
    if (!input || !sample_time(input->started_at) || !sample_time(input->collected_at) || input->phase != "operationResult" ||
        !sample_token(input->kind, {"connection", "campusVerification"}) ||
        !sample_token(input->result, {"operationFailed", "verificationFailed", "memoryWriteFailed", "identificationUnavailable"}) ||
        !sample_token(input->operation, {"manual", "automatic"}) ||
        !sample_token(input->monitoring_mode, {"manual", "observe", "recover"}) ||
        !sample_token(input->area, {"automatic", "teaching", "dormitory"}) ||
        !sample_token(input->status, {"idle", "checking", "discovering", "loggingIn", "verifying", "online", "offline", "confirming", "paused", "coolingDown",
            "unconfirmed", "needsTrust", "needsAccount", "accepted", "rejected", "uncertain", "security", "unavailable", "cancelled", "noNetwork", "sleeping"})) return std::nullopt;
    auto safe = *input;
    safe.environment = sample_environment(safe.environment);
    if (!sample_token(safe.campus_state, {"notChecked", "checking", "confirmed", "unconfirmed"})) safe.campus_state = "notChecked";
    if (safe.campus_state != "unconfirmed" || failure_issue_token(safe.campus_issue) == "none") safe.campus_issue = CampusVerificationIssue::none;
    if (!sample_token(safe.recovery.state, {"off", "needsAccount", "paused", "needsTrust", "coolingDown", "limited", "ready", "awaitingProof", "unknown"})) safe.recovery.state = "unknown";
    safe.recovery.hold_reasons.clear();
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials})
        if (std::find(input->recovery.hold_reasons.begin(), input->recovery.hold_reasons.end(), reason) != input->recovery.hold_reasons.end())
            safe.recovery.hold_reasons.push_back(reason);
    if (!safe.recovery.cooldown_remaining || !std::isfinite(*safe.recovery.cooldown_remaining) || *safe.recovery.cooldown_remaining <= 0)
        safe.recovery.cooldown_remaining.reset();
    else safe.recovery.cooldown_remaining = std::ceil(std::min(86400.0, *safe.recovery.cooldown_remaining));
    if (safe.physical_network && safe.physical_collected_at && sample_time(*safe.physical_collected_at))
        safe.physical_network = physical::anonymous_diagnostics(*safe.physical_network);
    else { safe.physical_network.reset(); safe.physical_collected_at.reset(); }
    return safe;
}
json::Value failure_context_json(const FailureContext& c, bool timestamps) {
    auto value = json::Value::object();
    if (timestamps) {
        value.set("startedAt", json::Value::string(iso8601(c.started_at)));
        value.set("collectedAt", json::Value::string(iso8601(c.collected_at)));
    }
    value.set("phase", json::Value::string(c.phase)); value.set("kind", json::Value::string(c.kind));
    value.set("result", json::Value::string(c.result)); value.set("operation", json::Value::string(c.operation));
    value.set("monitoringMode", json::Value::string(c.monitoring_mode)); value.set("area", json::Value::string(c.area));
    value.set("status", json::Value::string(c.status));
    auto environment = json::Value::object();
    environment.set("connection", json::Value::string(c.environment.connection));
    environment.set("systemProxy", json::Value::string(c.environment.system_proxy));
    environment.set("tun", json::Value::string(c.environment.tun));
    environment.set("physicalEgress", json::Value::string(c.environment.physical_egress));
    environment.set("refreshing", json::Value::boolean(c.environment.refreshing)); value.set("environment", std::move(environment));
    auto campus = json::Value::object(); campus.set("state", json::Value::string(c.campus_state));
    campus.set("issue", json::Value::string(std::string(failure_issue_token(c.campus_issue)))); value.set("campusVerification", std::move(campus));
    auto recovery = json::Value::object();
    recovery.set("state", json::Value::string(c.recovery.state)); recovery.set("readOnly", json::Value::boolean(c.recovery.read_only));
    recovery.set("paused", json::Value::boolean(c.recovery.paused)); recovery.set("networkAvailable", json::Value::boolean(c.recovery.network_available));
    recovery.set("networkRemembered", json::Value::boolean(c.recovery.network_remembered)); recovery.set("sessionTrusted", json::Value::boolean(c.recovery.session_trusted));
    auto holds = json::Value::array();
    for (const auto reason : c.recovery.hold_reasons) holds.push(json::Value::string(std::string(to_string(reason))));
    recovery.set("holdReasons", std::move(holds));
    if (c.recovery.cooldown_remaining) recovery.set("cooldownRemainingSeconds", json::Value::number(timestamps ? *c.recovery.cooldown_remaining : 1));
    value.set("recovery", std::move(recovery));
    if (c.physical_network) {
        auto physical = json::Value::object();
        physical.set("physicalInterfaceCount", json::Value::number(static_cast<double>(c.physical_network->interface_count)));
        physical.set("candidateCount", json::Value::number(static_cast<double>(c.physical_network->candidate_count)));
        auto rejections = json::Value::object();
        for (const auto& [reason, count] : c.physical_network->rejections) rejections.set(std::string(physical::to_string(reason)), json::Value::number(count));
        physical.set("rejections", std::move(rejections)); value.set("physicalNetwork", std::move(physical));
        if (timestamps) value.set("physicalCollectedAt", json::Value::string(iso8601(*c.physical_collected_at)));
    }
    return value;
}
std::optional<FailureContext> failure_context_from_json(const json::Value* value) {
    if (!value || !value->is_object()) return std::nullopt;
    const auto started = value->string_at("startedAt"), collected = value->string_at("collectedAt");
    const auto start = started ? parse_iso8601(*started) : std::nullopt, end = collected ? parse_iso8601(*collected) : std::nullopt;
    const auto* environment = value->find("environment"); const auto* campus = value->find("campusVerification"); const auto* recovery = value->find("recovery");
    if (!start || !end || !environment || !environment->is_object() || !campus || !campus->is_object() || !recovery || !recovery->is_object()) return std::nullopt;
    FailureContext c; c.started_at = *start; c.collected_at = *end;
    c.phase = value->string_at("phase").value_or(""); c.kind = value->string_at("kind").value_or(""); c.result = value->string_at("result").value_or("");
    c.operation = value->string_at("operation").value_or(""); c.monitoring_mode = value->string_at("monitoringMode").value_or("");
    c.area = value->string_at("area").value_or(""); c.status = value->string_at("status").value_or("");
    c.environment = {environment->string_at("connection").value_or("unknown"), environment->string_at("systemProxy").value_or("unknown"),
        environment->string_at("tun").value_or("unknown"), environment->bool_at("refreshing").value_or(false)};
    c.environment.physical_egress = environment->string_at("physicalEgress").value_or("unknown");
    c.campus_state = campus->string_at("state").value_or("notChecked");
    const auto issue = campus->string_at("issue").value_or("none");
    for (const auto candidate : {CampusVerificationIssue::none, CampusVerificationIssue::dnsTimeout, CampusVerificationIssue::dns, CampusVerificationIssue::dnsTls,
            CampusVerificationIssue::tls, CampusVerificationIssue::connection, CampusVerificationIssue::http, CampusVerificationIssue::page, CampusVerificationIssue::binding})
        if (failure_issue_token(candidate) == issue) c.campus_issue = candidate;
    c.recovery.state = recovery->string_at("state").value_or("unknown"); c.recovery.read_only = recovery->bool_at("readOnly").value_or(false);
    c.recovery.paused = recovery->bool_at("paused").value_or(false); c.recovery.network_available = recovery->bool_at("networkAvailable").value_or(false);
    c.recovery.network_remembered = recovery->bool_at("networkRemembered").value_or(false); c.recovery.session_trusted = recovery->bool_at("sessionTrusted").value_or(false);
    c.recovery.cooldown_remaining = recovery->number_at("cooldownRemainingSeconds");
    const auto* holds = recovery->find("holdReasons");
    if (holds && holds->is_array()) for (const auto& hold : holds->items())
        if (hold.is_string()) for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials})
            if (hold.as_string() == to_string(reason)) c.recovery.hold_reasons.push_back(reason);
    const auto* physical = value->find("physicalNetwork"); const auto observed = value->string_at("physicalCollectedAt");
    if (physical && physical->is_object() && observed) {
        const auto date = parse_iso8601(*observed);
        if (date) {
            physical::CandidateDiagnostics diagnostics;
            const auto count = [](std::optional<double> n) { return n && std::isfinite(*n) && *n >= 0 ? static_cast<unsigned>(std::min(4096.0, std::floor(*n))) : 0u; };
            diagnostics.interface_count = count(physical->number_at("physicalInterfaceCount")); diagnostics.candidate_count = count(physical->number_at("candidateCount"));
            const auto* rejected = physical->find("rejections");
            if (rejected && rejected->is_object()) for (int n = 0; n <= static_cast<int>(physical::CandidateRejection::noIPv4Service); ++n) {
                const auto reason = static_cast<physical::CandidateRejection>(n);
                const auto number = count(rejected->number_at(physical::to_string(reason)));
                if (number) diagnostics.reject(reason, number);
            }
            c.physical_network = std::move(diagnostics); c.physical_collected_at = *date;
        }
    }
    return safe_failure_context(c);
}
}

void HealthLog::append(const Sample& sample) {
    if (!sample_time(sample.time)) return;
    items_.push_back(safe_sample(sample));
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
        if (s.apple_ms) item.set("appleMilliseconds", json::Value::number(std::round(*s.apple_ms)));
        if (s.microsoft_ms) item.set("microsoftMilliseconds", json::Value::number(std::round(*s.microsoft_ms)));
        if (!s.diagnosis.empty()) item.set("diagnosis", json::Value::string(s.diagnosis));
        if (!s.complete_result.empty()) item.set("completeResult", json::Value::string(s.complete_result));
        if (s.context) item.set("context", context_json(*s.context));
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
        s.apple_ms = item.number_at("appleMilliseconds");
        s.microsoft_ms = item.number_at("microsoftMilliseconds");
        s.diagnosis = item.string_at("diagnosis").value_or("");
        s.complete_result = item.string_at("completeResult").value_or("");
        s.context = context_from_json(item.find("context"));
        const double total = item.number_at("websiteTotal").value_or(-1), passed = item.number_at("websitePassed").value_or(-1);
        if (total >= 0 && total <= 12 && passed >= 0 && passed <= total && total == std::floor(total) && passed == std::floor(passed)) {
            s.site_total = static_cast<int>(total);
            s.site_passed = static_cast<int>(passed);
        }
        log.append(s);
    }
    return log;
}

Sample sample_from(const ConnectivityReport& report, double now, int site_total, int site_passed,
                   const std::optional<SampleContext>& context) {
    Sample s;
    if (site_total >= 0 && site_total <= 12 && site_passed >= 0 && site_passed <= site_total) { s.site_total = site_total; s.site_passed = site_passed; }
    s.time = now;
    s.online = report.online();
    s.mode = std::string(to_string(report.mode));
    s.apple = std::string(to_string(report.apple.state));
    s.microsoft = std::string(to_string(report.microsoft.state));
    if (report.apple.state != EndpointState::notCompleted) s.apple_ms = report.apple.elapsed * 1000;
    if (report.microsoft.state != EndpointState::notCompleted) s.microsoft_ms = report.microsoft.elapsed * 1000;
    s.diagnosis = std::string(to_string(report.diagnosis()));
    s.context = context;
    if (report.complete_result) {
        switch (*report.complete_result) {
            case CompleteCheckResult::allPassed: s.complete_result = "allPassed"; break;
            case CompleteCheckResult::partial: s.complete_result = "partial"; break;
            case CompleteCheckResult::unavailable: s.complete_result = "unavailable"; break;
            case CompleteCheckResult::cancelled: s.complete_result = "cancelled"; break;
        }
    }
    double best = -1;
    for (const EndpointResult* r : {&report.apple, &report.microsoft})
        if (r->succeeded() && (best < 0 || r->elapsed * 1000 < best)) best = r->elapsed * 1000;
    s.latency_ms = best;
    return safe_sample(std::move(s));
}

std::string physical_identification_key(const physical::CandidateDiagnostics& input, bool identified) {
    const auto diagnostics = physical::anonymous_diagnostics(input);
    if (diagnostics.candidate_count) return identified ? "env.identification.ready" : "env.identification.awaitingRefresh";
    using Reason = physical::CandidateRejection;
    const Reason priority[]{Reason::systemSnapshotUnavailable, Reason::invalidDHCPServer, Reason::associatedDNSMismatch,
        Reason::missingDNS, Reason::invalidDNS, Reason::duplicateInterface, Reason::missingDefaultRoute,
        Reason::conflictingDefaultRoutes, Reason::gatewayMismatch, Reason::missingSourceAddress, Reason::invalidSourceAddress,
        Reason::invalidSubnet, Reason::invalidGateway, Reason::missingInterfaceIndex, Reason::missingDescriptor,
        Reason::unstableSnapshot, Reason::noIPv4Service, Reason::inactiveLink, Reason::unsupportedInterface};
    for (const auto reason : priority) {
        if (!diagnostics.rejections.count(reason)) continue;
        const auto key = reason == Reason::systemSnapshotUnavailable ? "interfaceSnapshotUnavailable" :
            reason == Reason::associatedDNSMismatch ? "invalidDNS" : reason == Reason::duplicateInterface ? "duplicateService" :
            std::string(physical::to_string(reason));
        return "env.identification.reason." + key;
    }
    return identified ? "env.identification.ready" : "env.identification.notCollected";
}

namespace {
bool known_campus_state(std::string_view state) {
    return state == "notChecked" || state == "checking" || state == "confirmed" || state == "unconfirmed";
}
std::string_view campus_issue_token(const ExportContext& c) {
    if (c.campus_state != "unconfirmed") return "none";
    switch (c.campus_issue) {
        case CampusVerificationIssue::dnsTimeout: return "dnsTimeout";
        case CampusVerificationIssue::dns: return "dns";
        case CampusVerificationIssue::dnsTls: return "dnsTls";
        case CampusVerificationIssue::tls: return "tls";
        case CampusVerificationIssue::connection: return "connection";
        case CampusVerificationIssue::http: return "http";
        case CampusVerificationIssue::page: return "page";
        case CampusVerificationIssue::binding: return "binding";
        case CampusVerificationIssue::none: return "none";
    }
    return "none";
}

std::string fixed_token(std::string_view value, std::initializer_list<std::string_view> known) {
    return std::find(known.begin(), known.end(), value) != known.end() ? std::string(value) : "unknown";
}
DiagnosticEnvironment anonymous_environment(const DiagnosticEnvironment& input) {
    DiagnosticEnvironment safe;
    safe.refreshing = input.refreshing;
    if (!safe.refreshing) {
        safe.connection = fixed_token(input.connection, {"wifi", "ethernet", "multiple", "tunnelHidden", "none", "unknown"});
        safe.system_proxy = fixed_token(input.system_proxy, {"on", "off", "unknown"});
        safe.tun = fixed_token(input.tun, {"detected", "notDetected", "unknown"});
        safe.physical_egress = fixed_token(input.physical_egress, {"normal", "forwarding", "weakHostSend", "forwardingAndWeakHostSend", "unknown"});
    }
    return safe;
}
std::string recovery_state_token(std::string_view value) {
    return fixed_token(value, {"off", "needsAccount", "paused", "needsTrust", "coolingDown", "limited", "ready", "awaitingProof", "unknown"});
}
std::vector<std::string> anonymous_hold_reasons(const std::vector<HoldReason>& input) {
    std::vector<std::string> safe;
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials})
        if (std::find(input.begin(), input.end(), reason) != input.end()) safe.emplace_back(to_string(reason));
    return safe;
}
std::optional<int> cooldown_seconds(const DiagnosticRecovery& input) {
    if (!input.cooldown_remaining || !std::isfinite(*input.cooldown_remaining) || *input.cooldown_remaining <= 0) return std::nullopt;
    return static_cast<int>(std::ceil(std::min(86400.0, *input.cooldown_remaining)));
}
std::string recovery_state_text(const ExportContext& context, const DiagnosticRecovery& recovery) {
    if (recovery.read_only) return std::string(copy::text("auto.readOnly.chip"));
    const auto state = recovery_state_token(recovery.state);
    if (state == "off") return std::string(copy::text(context.mode == "observe" ? "mode.observe.title" : "mode.manual.title"));
    if (state == "unknown") return std::string(copy::text("env.access.unknown"));
    return std::string(copy::text("auto." + state + ".chip"));
}
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
    auto retained_health = health;
    retained_health.prune(c.now);
    const json::Value health_json = retained_health.to_json();
    const json::Value events_json = events.to_json();
    root.set("samples", *health_json.find("samples"));
    root.set("events", *events_json.find("events"));
    root.set("privacy", json::Value::string(std::string(copy::text("records.exportPrivacy"))));
    if (known_campus_state(c.campus_state)) {
        json::Value campus = json::Value::object();
        campus.set("state", json::Value::string(c.campus_state));
        campus.set("issue", json::Value::string(std::string(campus_issue_token(c))));
        root.set("campusVerification", std::move(campus));
    }
    if (c.environment) {
        const auto safe = anonymous_environment(*c.environment);
        json::Value environment = json::Value::object();
        environment.set("connection", json::Value::string(safe.connection));
        environment.set("systemProxy", json::Value::string(safe.system_proxy));
        environment.set("tun", json::Value::string(safe.tun));
        environment.set("physicalEgress", json::Value::string(safe.physical_egress));
        environment.set("refreshing", json::Value::boolean(safe.refreshing));
        root.set("environment", std::move(environment));
    }
    if (c.recovery) {
        const auto& input = *c.recovery;
        json::Value recovery = json::Value::object();
        recovery.set("state", json::Value::string(recovery_state_token(input.state)));
        recovery.set("readOnly", json::Value::boolean(input.read_only));
        recovery.set("paused", json::Value::boolean(input.paused));
        recovery.set("networkAvailable", json::Value::boolean(input.network_available));
        recovery.set("networkRemembered", json::Value::boolean(input.network_remembered));
        recovery.set("sessionTrusted", json::Value::boolean(input.session_trusted));
        json::Value holds = json::Value::array();
        for (const auto& reason : anonymous_hold_reasons(input.hold_reasons)) holds.push(json::Value::string(reason));
        recovery.set("holdReasons", std::move(holds));
        if (const auto seconds = cooldown_seconds(input)) recovery.set("cooldownRemainingSeconds", json::Value::number(*seconds));
        root.set("recovery", std::move(recovery));
    }
    if (c.crashes) root.set("crashes", crash::to_json(*c.crashes));
    if (c.physical_network) {
        const auto safe = physical::anonymous_diagnostics(*c.physical_network);
        json::Value physical = json::Value::object();
        physical.set("physicalInterfaceCount", json::Value::number(static_cast<double>(safe.interface_count)));
        physical.set("candidateCount", json::Value::number(static_cast<double>(safe.candidate_count)));
        json::Value reasons = json::Value::object();
        for (const auto& [reason, count] : safe.rejections)
            reasons.set(std::string(physical::to_string(reason)), json::Value::number(count));
        physical.set("rejections", std::move(reasons));
        root.set("physicalNetwork", std::move(physical));
    }
    return root;
}

std::string diagnostics_summary_text(const ExportContext& c, const HealthLog& health) {
    const HealthSummary s = health.summary(c.now);
    std::string text = std::string(copy::text("app.name")) + " " + c.version + " · " + c.platform + "\n";
    text += copy::format("records.health.samples", {{"count", std::to_string(s.samples)}}) + " · " +
            copy::format("records.health.successRate", {{"percent", std::to_string(s.success_percent())}});
    if (s.median_latency_ms) text += " · " + copy::format("records.health.latency", {{"ms", std::to_string(static_cast<long>(std::lround(*s.median_latency_ms)))}});
    text += "\n" + std::string(copy::text("records.health.note"));
    std::size_t failures = 0;
    for (auto it = health.items().rbegin(); it != health.items().rend() && failures < 3; ++it) {
        const auto& sample = *it;
        if (c.now - sample.time > HealthLog::kWindowSeconds || sample.complete_result == "cancelled" ||
            sample.apple == "cancelled" || sample.microsoft == "cancelled") continue;
        if (sample.online && sample.mode != "complete") continue;
        const auto failed = [](const std::string& endpoint) { return endpoint != "success" && endpoint != "notCompleted"; };
        if (!failed(sample.apple) && !failed(sample.microsoft) && !(sample.site_total > 0 && sample.site_passed < sample.site_total)) continue;
        if (!failures++) text += "\n" + std::string(copy::text("records.diagnostics.recentFailures"));
        const auto endpoint = [](const std::string& state, const std::optional<double>& milliseconds) {
            std::string value(copy::text(state == "success" ? "endpoint.successTitle" : "endpoint." + state));
            if (milliseconds) value += " · " + std::to_string(static_cast<long>(std::lround(*milliseconds))) + " ms";
            return value;
        };
        const std::string reason = sample.online ? std::string(copy::text("records.sampleResult.partial")) :
            std::string(copy::text("diagnosis." + (sample.diagnosis.empty() ? "incomplete" : sample.diagnosis) + ".title"));
        text += "\n" + copy::format("records.diagnostics.failure", {{"time", iso8601(sample.time)}, {"reason", reason},
            {"apple", endpoint(sample.apple, sample.apple_ms)}, {"microsoft", endpoint(sample.microsoft, sample.microsoft_ms)}});
    }
    if (c.environment) {
        const auto safe = anonymous_environment(*c.environment);
        text += "\n" + copy::format("records.diagnostics.environment", {
            {"access", std::string(copy::text("env.access." + safe.connection))},
            {"proxy", std::string(copy::text("env.proxy." + safe.system_proxy))},
            {"tunnel", std::string(copy::text("env.tunnel." + safe.tun))}});
        text += "\n" + copy::format("records.diagnostics.physicalEgress", {
            {"state", std::string(copy::text("env.egress." + safe.physical_egress))}});
    }
    if (c.recovery) {
        const auto& recovery = *c.recovery;
        std::string holds;
        for (const auto& reason : anonymous_hold_reasons(recovery.hold_reasons)) {
            if (!holds.empty()) holds += "、";
            holds += copy::text("records.diagnostics.protection." + reason);
        }
        if (holds.empty()) holds = copy::text("records.diagnostics.protection.none");
        text += "\n" + copy::format("records.diagnostics.recovery", {{"state", recovery_state_text(c, recovery)}, {"holds", holds}});
        if (const auto seconds = cooldown_seconds(recovery))
            text += "\n" + copy::format("records.diagnostics.cooldown", {{"seconds", std::to_string(*seconds)}});
    }
    if (known_campus_state(c.campus_state))
        text += "\ncampus verification=" + c.campus_state + " issue=" + std::string(campus_issue_token(c));
    if (c.physical_network) {
        const auto safe = physical::anonymous_diagnostics(*c.physical_network);
        text += "\n" + copy::format("env.identification.summary", {{"reason", std::string(copy::text(physical_identification_key(safe, c.physical_identified)))}});
        text += "\nphysical interfaces=" + std::to_string(safe.interface_count) + " candidates=" + std::to_string(safe.candidate_count) + " reasons=";
        std::map<std::string, unsigned> sorted;
        for (const auto& [reason, count] : safe.rejections) sorted.emplace(physical::to_string(reason), count);
        if (sorted.empty()) text += "none";
        else for (auto it = sorted.begin(); it != sorted.end(); ++it)
            text += (it == sorted.begin() ? "" : ",") + it->first + "=" + std::to_string(it->second);
    }
    return text;
}

}  // namespace records
}  // namespace szu
