// Connection admission, evidence and scoped authentication holds.
// Mirrors macOS ConnectionPolicy.swift and tools/reference/recovery_reference.py.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "szu/netmemory.hpp"
#include "szu/types.hpp"

namespace szu {

enum class OperationKind { manual, automatic };
enum class HoldReason { userPaused, security, uncertain, credentials };  // declaration order = display priority
enum class HoldOrigin { explicitArea, candidate, global, user };

struct Hold {
    HoldReason reason = HoldReason::credentials;
    std::optional<CampusArea> area;           // nullopt: every area
    std::optional<std::uint64_t> generation;  // nullopt: any credentials
    std::optional<std::uint64_t> epoch;       // nullopt: survives network changes ("sticky")
    HoldOrigin origin = HoldOrigin::global;
    std::string rejection;                    // RejectionKind name, "invalidCredentials" or empty

    bool applies(CampusArea target, std::optional<std::uint64_t> gen, std::uint64_t current_epoch) const;
    bool sticky() const { return !epoch.has_value(); }
};

std::string_view to_string(OperationKind value);
std::string_view to_string(HoldReason value);
std::string_view to_string(HoldOrigin value);

// Physical interface identity only, held in RAM. IPs, SSIDs, DNS and TUN state never create a new scope.
struct AuthenticationScope {
    std::string kind, interface_key;
    bool valid() const { return (kind == "wifi" || kind == "ethernet") && !interface_key.empty(); }
    bool operator==(const AuthenticationScope&) const = default;
    bool operator<(const AuthenticationScope& other) const {
        return std::tie(kind, interface_key) < std::tie(other.kind, other.interface_key);
    }
};

class ConnectionPolicy {
public:
    static constexpr double kCooldownSeconds = 90;
    static constexpr int kStickyAfter = 2;

    struct Operation {
        std::uint64_t id = 0;
        OperationKind kind = OperationKind::manual;
    };

    bool available = false;
    bool foreground = true;
    bool automatic_enabled = false;

    // ---- admission ----
    std::optional<Operation> begin(OperationKind kind);
    bool owns(const Operation& op) const { return active_ && active_->id == op.id; }
    bool finish(const Operation& op);
    void cancel(std::optional<OperationKind> kind = std::nullopt);
    bool busy() const { return active_.has_value(); }
    std::optional<OperationKind> active_kind() const;
    bool can_run_automatic() const { return available && foreground && automatic_enabled; }

    // ---- probe evidence and cadence ----
    void observe_probe(bool online);
    int consecutive_probe_failures() const { return consecutive_probe_failures_; }
    void reset_probe_history() { consecutive_probe_failures_ = 0; }
    int stable_rounds() const { return stable_rounds_; }
    void failed() { failures_ = std::min(failures_ + 1, 4); }
    void reset_failures() { failures_ = 0; }
    int failures() const { return failures_; }
    int retry_delay() const { return std::min(300, 30 * (1 << std::min(failures_, 4))); }
    int next_check_delay(bool constrained, double now, bool paused) const;

    // ---- trust (session-scoped, never persisted) ----
    void authorize() { if (available && foreground) authorized_ = true; }
    void revoke_authorization() { authorized_ = false; }
    bool authorized() const { return authorized_; }
    void reset_for_path_change();

    // ---- duplicate-submission cooldown ----
    struct CooldownState {
        std::optional<double> not_before;
        std::optional<AuthenticationScope> accepted_scope;
        bool accepted_verified = false;
    };
    void attempt_started(double now);
    void accepted(double now, std::optional<AuthenticationScope> scope = std::nullopt);
    // A successful response alone is insufficient: complete bound connectivity must verify it.
    void verify_accepted_scope(const std::optional<AuthenticationScope>& scope, double now);
    bool cooling_down(double now) const { return cooldown_remaining(now).has_value(); }
    std::optional<double> cooldown_remaining(double now) const;
    std::optional<double> cooldown_remaining(double now, const std::optional<AuthenticationScope>& scope) const;
    CooldownState cooldown_state() const { return {not_before_, accepted_scope_, accepted_scope_verified_}; }
    void restore_cooldown_state(const CooldownState& state) {
        not_before_ = state.not_before; accepted_scope_ = state.accepted_scope; accepted_scope_verified_ = state.accepted_verified;
    }
    void restore_cooldown(double remaining, double now) {
        not_before_ = remaining > 0 ? std::optional<double>(now + std::min(remaining, kCooldownSeconds)) : std::nullopt;
        accepted_scope_.reset(); accepted_scope_verified_ = false; scoped_deadlines_.clear();
    }

    // ---- scoped holds ----
    const std::vector<Hold>& holds() const { return holds_; }
    void add(const Hold& hold);
    std::optional<Hold> record_failure(bool automatic_choice, CampusArea area, std::optional<std::uint64_t> generation,
                                       std::uint64_t epoch, const LoginAttempt& attempt);
    std::vector<Hold> record_success(CampusArea area, std::optional<std::uint64_t> generation, std::uint64_t epoch);
    bool settle_uncertain();
    void resume();  // clears every hold and failure counter; keeps the cooldown deadline
    void pause_by_user();
    void credentials_changed(std::uint64_t new_generation);
    void path_changed(std::uint64_t new_epoch);
    const Hold* blocking(CampusArea area, std::optional<std::uint64_t> generation, std::uint64_t epoch) const;
    const Hold* global_hold(std::optional<std::uint64_t> generation, std::uint64_t epoch) const;

private:
    std::optional<Operation> active_;
    std::uint64_t next_id_ = 1;
    int failures_ = 0;
    int consecutive_probe_failures_ = 0;
    int stable_rounds_ = 0;
    bool authorized_ = false;
    std::optional<double> not_before_;
    std::optional<AuthenticationScope> accepted_scope_;
    bool accepted_scope_verified_ = false;
    std::map<AuthenticationScope, double> scoped_deadlines_; // RAM only; durable recovery remains conservative/global.
    std::vector<Hold> holds_;
    std::map<std::pair<int, std::uint64_t>, int> candidate_counts_;
};

enum class PlanDecision { submit, unconfirmed, needsTrust, paused };
struct Plan {
    PlanDecision decision = PlanDecision::unconfirmed;
    CampusArea target = CampusArea::teaching;
};
std::string_view to_string(PlanDecision value);

// One submission target per round. Automatic rounds need confirmed discovery; the dormitory
// (plain HTTP) needs session trust unless the user explicitly asked (manual).
// `memory`/`ambiguous` describe a remembered-network match for this epoch. Without memory,
// dormitory recovery requires session trust and both HTTPS and portal confirmations.
Plan plan(OperationKind kind, CampusArea choice, const CampusDiscovery& discovery, bool trusted,
          const ConnectionPolicy& policy, std::optional<std::uint64_t> generation, std::uint64_t epoch,
          MatchLevel memory = MatchLevel::none, bool ambiguous = false);

enum class RoutingDecision { selected, unconfirmed, conflictingHints, needsSelection };
struct CampusRoutingResolution {
    RoutingDecision decision = RoutingDecision::selected;
    CampusArea choice = CampusArea::automatic;
    bool allow_unsent_fallback = true;
    bool require_client_config = false;
};
CampusRoutingResolution resolve_campus_routing(CampusArea choice, const ConnectivityReport& report,
                                             const CampusDiscovery& discovery, bool physically_bound,
                                             bool prefer_teaching = false, bool prefer_dormitory = false);

}  // namespace szu
