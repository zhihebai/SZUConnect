#include "szu/policy.hpp"

namespace szu {

bool Hold::applies(CampusArea target, std::optional<std::uint64_t> gen, std::uint64_t current_epoch) const {
    if (area && *area != target) return false;
    if (generation && (!gen || *generation != *gen)) return false;
    if (epoch && *epoch != current_epoch) return false;
    return true;
}

std::string_view to_string(OperationKind value) { return value == OperationKind::manual ? "manual" : "automatic"; }
std::string_view to_string(HoldReason value) {
    switch (value) {
        case HoldReason::userPaused: return "userPaused";
        case HoldReason::security: return "security";
        case HoldReason::uncertain: return "uncertain";
        case HoldReason::credentials: return "credentials";
    }
    return "credentials";
}
std::string_view to_string(HoldOrigin value) {
    switch (value) {
        case HoldOrigin::explicitArea: return "explicitArea";
        case HoldOrigin::candidate: return "candidate";
        case HoldOrigin::global: return "global";
        case HoldOrigin::user: return "user";
    }
    return "global";
}
std::string_view to_string(PlanDecision value) {
    switch (value) {
        case PlanDecision::submit: return "submit";
        case PlanDecision::unconfirmed: return "unconfirmed";
        case PlanDecision::needsTrust: return "needsTrust";
        case PlanDecision::paused: return "paused";
    }
    return "unconfirmed";
}

std::optional<ConnectionPolicy::Operation> ConnectionPolicy::begin(OperationKind kind) {
    if (!foreground || !available || active_) return std::nullopt;
    if (kind == OperationKind::automatic && !automatic_enabled) return std::nullopt;
    active_ = Operation{next_id_++, kind};
    return active_;
}

bool ConnectionPolicy::finish(const Operation& op) {
    if (!owns(op)) return false;
    active_.reset();
    return true;
}

void ConnectionPolicy::cancel(std::optional<OperationKind> kind) {
    if (!kind || (active_ && active_->kind == *kind)) active_.reset();
    reset_probe_history();
}

std::optional<OperationKind> ConnectionPolicy::active_kind() const {
    if (!active_) return std::nullopt;
    return active_->kind;
}

void ConnectionPolicy::observe_probe(bool online) {
    consecutive_probe_failures_ = online ? 0 : std::min(2, consecutive_probe_failures_ + 1);
    stable_rounds_ = online ? std::min(3, stable_rounds_ + 1) : 0;
    if (online) reset_failures();
}

int ConnectionPolicy::next_check_delay(bool constrained, double now, bool paused) const {
    if (not_before_ && *not_before_ > now) return 15;
    if (paused) return constrained ? 120 : 60;
    if (consecutive_probe_failures_ == 1) return 5;
    if (failures_ > 0) return retry_delay();
    if (constrained) return 120;
    if (stable_rounds_ < 2) return 30;
    return stable_rounds_ == 2 ? 45 : 60;
}

void ConnectionPolicy::reset_for_path_change() {
    cancel();
    revoke_authorization();
    reset_failures();
    stable_rounds_ = 0;
}

void ConnectionPolicy::attempt_started(double now) {
    not_before_ = now + kCooldownSeconds;
    accepted_scope_.reset(); accepted_scope_verified_ = false;
}

void ConnectionPolicy::accepted(double now, std::optional<AuthenticationScope> scope) {
    not_before_ = now + kCooldownSeconds;
    accepted_scope_ = scope && scope->valid() ? scope : std::nullopt;
    accepted_scope_verified_ = false;
    reset_probe_history();
}

void ConnectionPolicy::verify_accepted_scope(const std::optional<AuthenticationScope>& scope, double now) {
    if (!scope || !scope->valid() || scope != accepted_scope_ || !not_before_) return;
    for (auto it = scoped_deadlines_.begin(); it != scoped_deadlines_.end();)
        if (it->second <= now) it = scoped_deadlines_.erase(it); else ++it;
    // Never evict an active deadline to make room for another interface.
    if (!scoped_deadlines_.count(*scope) && scoped_deadlines_.size() >= 16) return;
    scoped_deadlines_[*scope] = *not_before_;
    accepted_scope_verified_ = true;
}

std::optional<double> ConnectionPolicy::cooldown_remaining(double now, const std::optional<AuthenticationScope>& scope) const {
    const auto global = cooldown_remaining(now);
    if (!global || !scope || !scope->valid() || !accepted_scope_ || !accepted_scope_verified_) return global;
    const auto found = scoped_deadlines_.find(*scope);
    if (found != scoped_deadlines_.end() && found->second > now) return found->second - now;
    // The previous request is settled, and this independently bound interface has no request settling.
    return scope == accepted_scope_ ? global : std::nullopt;
}

std::optional<double> ConnectionPolicy::cooldown_remaining(double now) const {
    if (!not_before_ || *not_before_ <= now) return std::nullopt;
    return *not_before_ - now;
}

void ConnectionPolicy::add(const Hold& hold) {
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(),
                                [&](const Hold& h) {
                                    return h.reason == hold.reason && h.area == hold.area &&
                                           h.generation == hold.generation && h.epoch == hold.epoch;
                                }),
                 holds_.end());
    holds_.push_back(hold);
}

std::optional<Hold> ConnectionPolicy::record_failure(bool automatic_choice, CampusArea area,
                                                     std::optional<std::uint64_t> generation, std::uint64_t epoch,
                                                     const LoginAttempt& attempt) {
    Hold hold;
    switch (attempt.outcome) {
        case Outcome::success:
        case Outcome::unavailable:
        case Outcome::challengeRefused:
        case Outcome::cancelled:
            return std::nullopt;
        case Outcome::security:
            hold = Hold{HoldReason::security, std::nullopt, std::nullopt, std::nullopt, HoldOrigin::global, {}};
            break;
        case Outcome::uncertain:
            hold = Hold{HoldReason::uncertain, std::nullopt, std::nullopt, std::nullopt, HoldOrigin::global, {}};
            break;
        case Outcome::invalidCredentials:
            if (!generation) return std::nullopt;
            hold = Hold{HoldReason::credentials, std::nullopt, generation, std::nullopt, HoldOrigin::explicitArea,
                        "invalidCredentials"};
            break;
        case Outcome::rejected: {
            // An unsaved draft never speaks for the saved account.
            if (!generation) return std::nullopt;
            const std::string kind(to_string(attempt.rejection));
            if (automatic_choice && attempt.rejection != RejectionKind::password) {
                int& count = candidate_counts_[{static_cast<int>(area), *generation}];
                count += 1;
                // An area mismatch describes the network, never the credentials: it must not outlive the network.
                const bool sticky = attempt.rejection != RejectionKind::areaMismatch && count >= kStickyAfter;
                hold = Hold{HoldReason::credentials, area, generation,
                            sticky ? std::nullopt : std::optional<std::uint64_t>(epoch), HoldOrigin::candidate, kind};
            } else {
                hold = Hold{HoldReason::credentials, area, generation, std::nullopt,
                            automatic_choice ? HoldOrigin::candidate : HoldOrigin::explicitArea, kind};
            }
            break;
        }
    }
    add(hold);
    return hold;
}

std::vector<Hold> ConnectionPolicy::record_success(CampusArea area, std::optional<std::uint64_t> generation,
                                                   std::uint64_t epoch) {
    std::vector<Hold> released;
    if (!generation) return released;
    std::vector<Hold> kept;
    for (const Hold& h : holds_) {
        const bool same_generation = h.generation == generation;
        const bool release =
            (same_generation && h.area == area && h.reason != HoldReason::userPaused) ||
            (!h.area && !h.generation && (h.reason == HoldReason::security || h.reason == HoldReason::uncertain)) ||
            (same_generation && h.origin == HoldOrigin::candidate && h.epoch == epoch) ||
            (same_generation && !h.area && h.reason == HoldReason::credentials);
        (release ? released : kept).push_back(h);
    }
    holds_ = std::move(kept);
    for (const Hold& h : released) {
        if (h.area) candidate_counts_.erase({static_cast<int>(*h.area), *generation});
    }
    candidate_counts_.erase({static_cast<int>(area), *generation});
    return released;
}

bool ConnectionPolicy::settle_uncertain() {
    const std::size_t before = holds_.size();
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(), [](const Hold& h) { return h.reason == HoldReason::uncertain; }),
                 holds_.end());
    return before != holds_.size();
}

void ConnectionPolicy::resume() {
    holds_.clear();
    candidate_counts_.clear();
    // Preserve not_before_: resuming must not bypass duplicate-submission protection.
    reset_failures();
    reset_probe_history();
}

void ConnectionPolicy::pause_by_user() {
    add(Hold{HoldReason::userPaused, std::nullopt, std::nullopt, std::nullopt, HoldOrigin::user, {}});
}

void ConnectionPolicy::credentials_changed(std::uint64_t new_generation) {
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(),
                                [&](const Hold& h) { return h.generation && *h.generation != new_generation; }),
                 holds_.end());
    for (auto it = candidate_counts_.begin(); it != candidate_counts_.end();) {
        if (it->first.second != new_generation) it = candidate_counts_.erase(it);
        else ++it;
    }
}

void ConnectionPolicy::path_changed(std::uint64_t new_epoch) {
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(), [&](const Hold& h) { return h.epoch && *h.epoch != new_epoch; }),
                 holds_.end());
}

namespace {
const Hold* best(const std::vector<const Hold*>& found) {
    const Hold* result = nullptr;
    for (const Hold* h : found) {
        if (!result || static_cast<int>(h->reason) < static_cast<int>(result->reason)) result = h;
    }
    return result;
}
}  // namespace

const Hold* ConnectionPolicy::blocking(CampusArea area, std::optional<std::uint64_t> generation, std::uint64_t epoch) const {
    std::vector<const Hold*> found;
    for (const Hold& h : holds_)
        if (h.applies(area, generation, epoch)) found.push_back(&h);
    return best(found);
}

const Hold* ConnectionPolicy::global_hold(std::optional<std::uint64_t> generation, std::uint64_t epoch) const {
    std::vector<const Hold*> found;
    for (const Hold& h : holds_)
        if (!h.area && h.applies(CampusArea::teaching, generation, epoch)) found.push_back(&h);
    return best(found);
}

Plan plan(OperationKind kind, CampusArea choice, const CampusDiscovery& discovery, bool trusted,
          const ConnectionPolicy& policy, std::optional<std::uint64_t> generation, std::uint64_t epoch, MatchLevel memory, bool ambiguous) {
    const bool remembered = memory_verdict(memory, discovery, ambiguous) == MemoryVerdict::authorized;
    const bool teaching_ok = discovery.teaching == Discovery::confirmed;
    const bool dorm_ok = discovery.dormitory == Discovery::confirmed;
    const bool manual = kind == OperationKind::manual;
    auto blocked = [&](CampusArea area) { return !manual && policy.blocking(area, generation, epoch) != nullptr; };
    auto submit = [](CampusArea area) { return Plan{PlanDecision::submit, area}; };
    const Plan paused{PlanDecision::paused, CampusArea::teaching};
    const Plan unconfirmed{PlanDecision::unconfirmed, CampusArea::teaching};
    const Plan needs_trust{PlanDecision::needsTrust, CampusArea::dormitory};

    if (choice == CampusArea::teaching) {
        if (!manual && !teaching_ok) return unconfirmed;
        return blocked(CampusArea::teaching) ? paused : submit(CampusArea::teaching);
    }
    if (choice == CampusArea::dormitory) {
        if (!manual && !((teaching_ok && dorm_ok) || remembered)) return unconfirmed;
        if (blocked(CampusArea::dormitory)) return paused;
        if (!manual && !(trusted || remembered)) return needs_trust;
        return submit(CampusArea::dormitory);
    }
    if (!teaching_ok) {
        // A remembered network with a fresh dormitory proof does not need the teaching service to be up.
        if (remembered) return blocked(CampusArea::dormitory) ? paused : submit(CampusArea::dormitory);
        return unconfirmed;
    }
    if (dorm_ok) {
        if (!blocked(CampusArea::dormitory)) return (manual || trusted || remembered) ? submit(CampusArea::dormitory) : needs_trust;
        if (!blocked(CampusArea::teaching)) return submit(CampusArea::teaching);
        return paused;
    }
    return blocked(CampusArea::teaching) ? paused : submit(CampusArea::teaching);
}

}  // namespace szu
