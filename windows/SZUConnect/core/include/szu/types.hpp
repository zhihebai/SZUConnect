// Shared vocabulary of the SZUConnect engine. Names mirror the macOS Swift types and the
// shared fixtures so both platforms are tested against the same JSON files.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace szu {

enum class CampusArea { automatic, teaching, dormitory };
enum class MonitoringMode { manual, observe, recover };
enum class ProbeMode { quick, complete };
enum class Discovery { confirmed, unconfirmed, notChecked };

struct CampusDiscovery {
    Discovery teaching = Discovery::unconfirmed;
    Discovery dormitory = Discovery::notChecked;
};

// Outcome of one authentication attempt against ONE concrete area.
enum class Outcome {
    success,
    unavailable,        // availability failure before any credential left the machine
    challengeRefused,   // SRun refused the challenge: no password was sent
    security,           // TLS / identity / redirect / protocol conflict before submission
    rejected,           // password was sent and explicitly refused
    uncertain,          // password was sent, result unknown
    invalidCredentials, // local validation
    cancelled
};

enum class RejectionKind { password, account, areaMismatch, unspecified };

enum class PortalError {
    none, invalidCredentials, network, response, rejected, cryptography, forbiddenTarget, tls,
    timeout, dns, redirect, httpStatus, responseTooLarge, authenticationUncertain, protocolConflict,
    challengeRefused
};

struct LoginAttempt {
    Outcome outcome = Outcome::unavailable;
    RejectionKind rejection = RejectionKind::unspecified;
    PortalError error = PortalError::none;
    std::string message;  // credential-free copy key, e.g. "event.succeededTeaching"
    bool credential_submission_started = true; // false only with positive proof no credential bytes were sent
};

enum class EndpointState { success, timeout, dns, tls, redirect, unexpectedResponse, network, cancelled, notCompleted };

struct EndpointResult {
    EndpointState state = EndpointState::notCompleted;
    double elapsed = 0;
    bool succeeded() const { return state == EndpointState::success; }
};

enum class Diagnosis { online, portal, response, security, dns, timeout, unreachable, incomplete };

// Aggregate complete-check evidence contains no website identity, error or Location.
// Recovery candidates and transport recovery are separate RAM-only facts.
enum class CompleteCheckResult { allPassed, partial, unavailable, cancelled };

struct ConnectivityReport {
    EndpointResult apple;
    EndpointResult microsoft;
    ProbeMode mode = ProbeMode::quick;
    std::optional<CompleteCheckResult> complete_result;  // absent in legacy fakes/fixtures
    std::optional<bool> recovery_evidence;
    std::optional<bool> selected_transport_recovered;
    std::optional<bool> campus_recovery_evidence;
    bool websites_deferred = false;
    bool complete_base_passed() const {
        return mode == ProbeMode::complete && all_passed() && complete_result != CompleteCheckResult::cancelled;
    }
    bool campus_recoverable_partial() const {
        return complete_partial() && online() && campus_recovery_evidence.value_or(!all_passed());
    }
    bool complete_partial() const;
    bool website_only_issues() const;
    bool recoverable_partial() const;
    bool has_recovery_evidence() const;
    bool transport_recovered() const;
    bool complete_all_passed() const;
    bool online() const { return apple.succeeded() || microsoft.succeeded(); }
    bool all_passed() const { return apple.succeeded() && microsoft.succeeded(); }
    int passed_count() const { return (apple.succeeded() ? 1 : 0) + (microsoft.succeeded() ? 1 : 0); }
    Diagnosis diagnosis() const;
    static ConnectivityReport simple(bool apple_ok, bool microsoft_ok);
};

struct Credentials {
    std::string account;
    std::string password;
    bool complete() const;  // trimmed account non-empty, password non-empty, within size limits
    friend bool operator==(const Credentials& a, const Credentials& b) {
        return a.account == b.account && a.password == b.password;
    }
    friend bool operator!=(const Credentials& a, const Credentials& b) { return !(a == b); }
};

std::string trim(std::string_view text);  // ASCII whitespace, U+00A0 and U+3000 at both ends

std::string_view to_string(CampusArea value);
std::string_view to_string(MonitoringMode value);
std::string_view to_string(ProbeMode value);
std::string_view to_string(Discovery value);
std::string_view to_string(Outcome value);
std::string_view to_string(RejectionKind value);
std::string_view to_string(EndpointState value);
std::string_view to_string(Diagnosis value);
std::string_view to_string(PortalError value);

std::optional<CampusArea> area_from(std::string_view text);
std::optional<MonitoringMode> mode_from(std::string_view text);
std::optional<ProbeMode> probe_mode_from(std::string_view text);
std::optional<Discovery> discovery_from(std::string_view text);
std::optional<EndpointState> endpoint_state_from(std::string_view text);

// Same migration as macOS MonitoringMode.migrated(raw:legacyAutoReconnect:).
MonitoringMode migrated_mode(std::optional<std::string_view> raw, bool legacy_auto_reconnect);

}  // namespace szu
