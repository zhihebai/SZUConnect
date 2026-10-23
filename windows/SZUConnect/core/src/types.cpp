#include "szu/types.hpp"

namespace szu {

bool ConnectivityReport::complete_partial() const {
    return mode == ProbeMode::complete && online() && complete_result != CompleteCheckResult::cancelled &&
        (!all_passed() || complete_result == CompleteCheckResult::partial);
}

bool ConnectivityReport::website_only_issues() const {
    return complete_partial() && all_passed();
}

bool ConnectivityReport::recoverable_partial() const {
    return has_recovery_evidence();
}

bool ConnectivityReport::has_recovery_evidence() const {
    return complete_partial() && (!all_passed() || recovery_evidence.value_or(false));
}

bool ConnectivityReport::transport_recovered() const {
    return mode == ProbeMode::complete && all_passed() && complete_result != CompleteCheckResult::cancelled &&
        selected_transport_recovered.value_or(complete_all_passed());
}

bool ConnectivityReport::complete_all_passed() const {
    return mode == ProbeMode::complete && all_passed() &&
        (!complete_result || complete_result == CompleteCheckResult::allPassed);
}

Diagnosis ConnectivityReport::diagnosis() const {
    auto has = [&](EndpointState s) { return apple.state == s || microsoft.state == s; };
    if (has(EndpointState::success)) return Diagnosis::online;
    if (has(EndpointState::redirect)) return Diagnosis::portal;
    if (has(EndpointState::tls)) return Diagnosis::security;
    if (has(EndpointState::unexpectedResponse)) return Diagnosis::response;
    if (has(EndpointState::dns)) return Diagnosis::dns;
    if (has(EndpointState::timeout)) return Diagnosis::timeout;
    if (has(EndpointState::network)) return Diagnosis::unreachable;
    return Diagnosis::incomplete;
}

ConnectivityReport ConnectivityReport::simple(bool apple_ok, bool microsoft_ok) {
    ConnectivityReport report;
    report.apple.state = apple_ok ? EndpointState::success : EndpointState::network;
    report.microsoft.state = microsoft_ok ? EndpointState::success : EndpointState::network;
    return report;
}

namespace {
std::size_t leading_space(std::string_view t, std::size_t i) {
    const auto c = static_cast<unsigned char>(t[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 1;
    if (c == 0xC2 && i + 1 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0xA0) return 2;
    if (c == 0xE3 && i + 2 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0x80 &&
        static_cast<unsigned char>(t[i + 2]) == 0x80) return 3;
    return 0;
}
std::size_t trailing_space(std::string_view t, std::size_t end, std::size_t begin) {
    const auto c = static_cast<unsigned char>(t[end - 1]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 1;
    if (end - begin >= 2 && static_cast<unsigned char>(t[end - 2]) == 0xC2 && c == 0xA0) return 2;
    if (end - begin >= 3 && static_cast<unsigned char>(t[end - 3]) == 0xE3 &&
        static_cast<unsigned char>(t[end - 2]) == 0x80 && c == 0x80) return 3;
    return 0;
}
}  // namespace

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t w = leading_space(text, begin);
        if (!w) break;
        begin += w;
    }
    std::size_t end = text.size();
    while (end > begin) {
        const std::size_t w = trailing_space(text, end, begin);
        if (!w) break;
        end -= w;
    }
    return std::string(text.substr(begin, end - begin));
}

bool Credentials::complete() const {
    const std::string trimmed = trim(account);
    return !trimmed.empty() && !password.empty() && trimmed.size() <= 256 && password.size() <= 1024;
}

std::string_view to_string(CampusArea value) {
    switch (value) {
        case CampusArea::automatic: return "automatic";
        case CampusArea::teaching: return "teaching";
        case CampusArea::dormitory: return "dormitory";
    }
    return "automatic";
}
std::string_view to_string(MonitoringMode value) {
    switch (value) {
        case MonitoringMode::manual: return "manual";
        case MonitoringMode::observe: return "observe";
        case MonitoringMode::recover: return "recover";
    }
    return "manual";
}
std::string_view to_string(ProbeMode value) { return value == ProbeMode::quick ? "quick" : "complete"; }
std::string_view to_string(Discovery value) {
    switch (value) {
        case Discovery::confirmed: return "confirmed";
        case Discovery::unconfirmed: return "unconfirmed";
        case Discovery::notChecked: return "notChecked";
    }
    return "unconfirmed";
}
std::string_view to_string(Outcome value) {
    switch (value) {
        case Outcome::success: return "success";
        case Outcome::unavailable: return "unavailable";
        case Outcome::challengeRefused: return "challengeRefused";
        case Outcome::security: return "security";
        case Outcome::rejected: return "rejected";
        case Outcome::uncertain: return "uncertain";
        case Outcome::invalidCredentials: return "invalidCredentials";
        case Outcome::cancelled: return "cancelled";
    }
    return "unavailable";
}
std::string_view to_string(RejectionKind value) {
    switch (value) {
        case RejectionKind::password: return "password";
        case RejectionKind::account: return "account";
        case RejectionKind::areaMismatch: return "areaMismatch";
        case RejectionKind::unspecified: return "unspecified";
    }
    return "unspecified";
}
std::string_view to_string(EndpointState value) {
    switch (value) {
        case EndpointState::success: return "success";
        case EndpointState::timeout: return "timeout";
        case EndpointState::dns: return "dns";
        case EndpointState::tls: return "tls";
        case EndpointState::redirect: return "redirect";
        case EndpointState::unexpectedResponse: return "unexpectedResponse";
        case EndpointState::network: return "network";
        case EndpointState::cancelled: return "cancelled";
        case EndpointState::notCompleted: return "notCompleted";
    }
    return "network";
}
std::string_view to_string(Diagnosis value) {
    switch (value) {
        case Diagnosis::online: return "online";
        case Diagnosis::portal: return "portal";
        case Diagnosis::response: return "response";
        case Diagnosis::security: return "security";
        case Diagnosis::dns: return "dns";
        case Diagnosis::timeout: return "timeout";
        case Diagnosis::unreachable: return "unreachable";
        case Diagnosis::incomplete: return "incomplete";
    }
    return "incomplete";
}
std::string_view to_string(PortalError value) {
    switch (value) {
        case PortalError::none: return "none";
        case PortalError::invalidCredentials: return "invalidCredentials";
        case PortalError::network: return "network";
        case PortalError::response: return "response";
        case PortalError::rejected: return "rejected";
        case PortalError::cryptography: return "cryptography";
        case PortalError::forbiddenTarget: return "forbiddenTarget";
        case PortalError::tls: return "tls";
        case PortalError::timeout: return "timeout";
        case PortalError::dns: return "dns";
        case PortalError::redirect: return "redirect";
        case PortalError::httpStatus: return "httpStatus";
        case PortalError::responseTooLarge: return "responseTooLarge";
        case PortalError::authenticationUncertain: return "authenticationUncertain";
        case PortalError::protocolConflict: return "protocolConflict";
        case PortalError::challengeRefused: return "challengeRefused";
    }
    return "none";
}

std::optional<CampusArea> area_from(std::string_view text) {
    for (CampusArea a : {CampusArea::automatic, CampusArea::teaching, CampusArea::dormitory})
        if (to_string(a) == text) return a;
    return std::nullopt;
}
std::optional<MonitoringMode> mode_from(std::string_view text) {
    for (MonitoringMode m : {MonitoringMode::manual, MonitoringMode::observe, MonitoringMode::recover})
        if (to_string(m) == text) return m;
    return std::nullopt;
}
std::optional<ProbeMode> probe_mode_from(std::string_view text) {
    if (text == "quick") return ProbeMode::quick;
    if (text == "complete") return ProbeMode::complete;
    return std::nullopt;
}
std::optional<Discovery> discovery_from(std::string_view text) {
    for (Discovery d : {Discovery::confirmed, Discovery::unconfirmed, Discovery::notChecked})
        if (to_string(d) == text) return d;
    return std::nullopt;
}
std::optional<EndpointState> endpoint_state_from(std::string_view text) {
    for (EndpointState s : {EndpointState::success, EndpointState::timeout, EndpointState::dns, EndpointState::tls,
                            EndpointState::redirect, EndpointState::unexpectedResponse, EndpointState::network,
                            EndpointState::cancelled, EndpointState::notCompleted})
        if (to_string(s) == text) return s;
    return std::nullopt;
}

MonitoringMode migrated_mode(std::optional<std::string_view> raw, bool legacy_auto_reconnect) {
    if (raw) {
        if (auto mode = mode_from(*raw)) return *mode;
    }
    return legacy_auto_reconnect ? MonitoringMode::recover : MonitoringMode::manual;
}

}  // namespace szu
