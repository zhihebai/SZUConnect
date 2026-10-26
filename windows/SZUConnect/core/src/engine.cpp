#include "szu/engine.hpp"

#include <algorithm>
#include <utility>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include "szu/json.hpp"

#include "../generated/shared_copy.hpp"

namespace szu {

// ---------------- CancelToken ----------------
struct CancelToken::State {
    std::mutex mutex;
    std::condition_variable changed;
    bool cancelled = false;
    std::uint64_t next = 1;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> callbacks;
};

CancelToken::CancelToken() : state_(std::make_shared<State>()) {}

void CancelToken::cancel() const {
    std::vector<std::pair<std::uint64_t, std::function<void()>>> callbacks;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->cancelled) return;
        state_->cancelled = true;
        callbacks.swap(state_->callbacks);
    }
    state_->changed.notify_all();
    for (auto& entry : callbacks) entry.second();
}

bool CancelToken::cancelled() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->cancelled;
}

bool CancelToken::wait_for(double seconds) const {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return state_->cancelled; });
}

std::uint64_t CancelToken::on_cancel(std::function<void()> fn) const {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->cancelled) {
            const std::uint64_t id = state_->next++;
            state_->callbacks.emplace_back(id, std::move(fn));
            return id;
        }
    }
    fn();
    return 0;
}

void CancelToken::remove(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& cbs = state_->callbacks;
    cbs.erase(std::remove_if(cbs.begin(), cbs.end(), [&](const auto& e) { return e.first == id; }), cbs.end());
}

// ---------------- names ----------------
std::string_view to_string(EngineStatus value) {
    switch (value) {
        case EngineStatus::idle: return "idle";
        case EngineStatus::checking: return "checking";
        case EngineStatus::discovering: return "discovering";
        case EngineStatus::loggingIn: return "loggingIn";
        case EngineStatus::verifying: return "verifying";
        case EngineStatus::online: return "online";
        case EngineStatus::offline: return "offline";
        case EngineStatus::confirming: return "confirming";
        case EngineStatus::paused: return "paused";
        case EngineStatus::coolingDown: return "coolingDown";
        case EngineStatus::unconfirmed: return "unconfirmed";
        case EngineStatus::needsTrust: return "needsTrust";
        case EngineStatus::needsAccount: return "needsAccount";
        case EngineStatus::accepted: return "accepted";
        case EngineStatus::rejected: return "rejected";
        case EngineStatus::uncertain: return "uncertain";
        case EngineStatus::security: return "security";
        case EngineStatus::unavailable: return "unavailable";
        case EngineStatus::cancelled: return "cancelled";
        case EngineStatus::noNetwork: return "noNetwork";
        case EngineStatus::sleeping: return "sleeping";
    }
    return "idle";
}

std::string_view to_string(AutoLoginState value) {
    switch (value) {
        case AutoLoginState::off: return "off";
        case AutoLoginState::needsAccount: return "needsAccount";
        case AutoLoginState::paused: return "paused";
        case AutoLoginState::needsTrust: return "needsTrust";
        case AutoLoginState::coolingDown: return "coolingDown";
        case AutoLoginState::limited: return "limited";
        case AutoLoginState::ready: return "ready";
        case AutoLoginState::awaitingProof: return "awaitingProof";
    }
    return "off";
}

std::string mask_account(const std::string& account) {
    const bool ascii = std::all_of(account.begin(), account.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (!ascii || account.size() <= 4) return "••••";
    return account.substr(0, 2) + "••••" + account.substr(account.size() - 2);
}

namespace {
std::optional<CampusPortalHint> physical_hint(const EndpointResult& endpoint) {
    return endpoint.state == EndpointState::redirect ? endpoint.campus_area_hint : std::nullopt;
}
CampusArea discovery_scope(CampusArea choice, const ConnectivityReport& report, bool bound, bool prefer_teaching) {
    if (choice == CampusArea::teaching) return CampusArea::teaching;
    if (choice != CampusArea::automatic) return CampusArea::automatic;
    const bool teaching = bound && (physical_hint(report.apple) == CampusPortalHint::teaching ||
        physical_hint(report.microsoft) == CampusPortalHint::teaching);
    const bool dormitory = bound && (physical_hint(report.apple) == CampusPortalHint::dormitory ||
        physical_hint(report.microsoft) == CampusPortalHint::dormitory);
    // A current teaching-only candidate needs HTTPS identity, not an unrelated
    // dormitory timeout. Explicit dormitory and conflicting hints still read both.
    return !dormitory && (teaching || prefer_teaching) ? CampusArea::teaching : CampusArea::automatic;
}
bool refresh_selection_hints(CampusArea area, const ConnectivityReport& report) {
    return area == CampusArea::automatic && (physical_hint(report.apple) || physical_hint(report.microsoft));
}
std::string area_text(std::optional<CampusArea> area) {
    if (!area) return std::string(copy::text("hold.area.all"));
    return std::string(copy::text(*area == CampusArea::dormitory ? "hold.area.dormitory" : "hold.area.teaching"));
}
}  // namespace

// ---------------- engine ----------------
ConnectionEngine::ConnectionEngine(ConnectionService& service, Hooks hooks, bool read_only_session)
    : service_(service), hooks_(std::move(hooks)), read_only_(read_only_session) {
    if (!hooks_.now) hooks_.now = [] { return 0.0; };
    if (!hooks_.wall_now) hooks_.wall_now = hooks_.now;
    if (!hooks_.sleep) hooks_.sleep = [](double) {};
}

void ConnectionEngine::emit(std::string text) {
    pending_events_.push_back(std::move(text));
    pending_change_ = true;
}

void ConnectionEngine::flush() {
    std::vector<std::string> events;
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recovery_initialized_ && !persist_recovery_locked()) {
            status_ = EngineStatus::security; pending_change_ = true;
        }
        events.swap(pending_events_);
        changed = pending_change_ || !events.empty();
        pending_change_ = false;
    }
    if (hooks_.event)
        for (const auto& e : events) hooks_.event(e);
    if (changed && hooks_.changed) hooks_.changed();
}

void ConnectionEngine::configure(MonitoringMode mode, CampusArea area, ProbeMode probe, std::optional<Credentials> saved) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((area_ != area && policy_.busy()) || policy_.active_kind() == OperationKind::automatic ||
            (mode != MonitoringMode::recover && automatic_campus_verification_operation_)) cancel_locked(false);
        if (area_ != area || (mode_ == MonitoringMode::recover && mode != MonitoringMode::recover))
            invalidate_campus_service_locked();
        mode_ = mode;
        area_ = area;
        probe_mode_ = probe;
        session_audit_after_.reset();
        saved_ = std::move(saved);
        if (saved_ && !saved_->complete()) saved_.reset();
        draft_ = saved_.value_or(Credentials{});
        generation_ = saved_ ? 1 : 0;
        reset_partial_episode_locked();
        policy_.automatic_enabled = mode_ != MonitoringMode::manual;
        pending_change_ = true;
    }
    flush();
}

// Durable holds never store network identity or credentials. Candidate epoch holds expire at restart.
bool ConnectionEngine::restore_recovery(const std::optional<std::string>& journal) {
    std::lock_guard<std::mutex> lock(mutex_);
    recovery_initialized_ = true;
    if (!journal) return true;
    auto fail = [&] {
        Hold hold; hold.reason = HoldReason::security; policy_.add(hold);
        recovery_write_blocked_ = true; pending_change_ = true;
        return false;
    };
    const auto value = json::parse(*journal, {.max_depth = 12, .max_bytes = 65536});
    if (!value || !value->is_object() || value->number_at("schema") != 1) return fail();
    const auto generation = value->number_at("generation");
    const auto* entries = value->find("holds");
    if (!generation || *generation < 0 || *generation > 9007199254740991.0 || std::floor(*generation) != *generation ||
        !entries || !entries->is_array() || entries->size() > 32) return fail();
    std::vector<Hold> loaded;
    for (const auto& entry : entries->items()) {
        if (!entry.is_object()) return fail();
        const auto reason = entry.number_at("reason"), origin = entry.number_at("origin");
        if (!reason || *reason < 0 || *reason > 3 || std::floor(*reason) != *reason ||
            !origin || *origin < 0 || *origin > 3 || std::floor(*origin) != *origin) return fail();
        Hold hold; hold.reason = static_cast<HoldReason>(static_cast<int>(*reason));
        hold.origin = static_cast<HoldOrigin>(static_cast<int>(*origin));
        if (const auto* area = entry.find("area")) {
            if (!area->is_string() || (area->as_string() != "teaching" && area->as_string() != "dormitory")) return fail();
            hold.area = area->as_string() == "teaching" ? CampusArea::teaching : CampusArea::dormitory;
        }
        if (const auto* gen = entry.find("generation")) {
            if (!gen->is_number() || gen->as_number() < 0 || gen->as_number() > 9007199254740991.0 ||
                std::floor(gen->as_number()) != gen->as_number()) return fail();
            hold.generation = static_cast<std::uint64_t>(gen->as_number());
        }
        loaded.push_back(hold);
    }
    std::optional<double> deadline;
    if (const auto* date = value->find("notBefore")) {
        if (!date->is_number() || !std::isfinite(date->as_number())) return fail();
        deadline = std::min(date->as_number(), hooks_.wall_now() + 90);
    }
    generation_ = static_cast<std::uint64_t>(*generation);
    for (const auto& hold : loaded) policy_.add(hold);
    if (deadline) {
        policy_.restore_cooldown(*deadline - hooks_.wall_now(), now());
        recovery_deadline_wall_ = deadline;
    }
    last_recovery_json_ = *journal;
    return true;
}

bool ConnectionEngine::persist_recovery_locked() {
    if (read_only_) return true;
    if (!hooks_.persist_recovery) return true; // In-memory test engines do not use storage.
    auto fail = [&] {
        Hold hold; hold.reason = HoldReason::security; policy_.add(hold);
        return false;
    };
    if (!recovery_initialized_ || recovery_write_blocked_) return fail();
    json::Value value = json::Value::object(), entries = json::Value::array();
    value.set("schema", json::Value::number(1));
    value.set("generation", json::Value::number(static_cast<double>(generation_)));
    bool uncertain = false;
    for (const auto& hold : policy_.holds()) {
        if (hold.epoch) continue;
        json::Value entry = json::Value::object();
        entry.set("reason", json::Value::number(static_cast<int>(hold.reason)));
        entry.set("origin", json::Value::number(static_cast<int>(hold.origin)));
        if (hold.area) entry.set("area", json::Value::string(std::string(to_string(*hold.area))));
        if (hold.generation) entry.set("generation", json::Value::number(static_cast<double>(*hold.generation)));
        entries.push(std::move(entry));
        uncertain = uncertain || hold.reason == HoldReason::uncertain;
    }
    if (recovery_submission_pending_ && !uncertain) {
        json::Value entry = json::Value::object();
        entry.set("reason", json::Value::number(static_cast<int>(HoldReason::uncertain)));
        entry.set("origin", json::Value::number(static_cast<int>(HoldOrigin::global)));
        entries.push(std::move(entry));
    }
    value.set("holds", std::move(entries));
    if (recovery_deadline_wall_) value.set("notBefore", json::Value::number(*recovery_deadline_wall_));
    const auto data = json::serialize(value);
    if (data == last_recovery_json_) return true;
    if (!hooks_.persist_recovery(data)) return fail();
    last_recovery_json_ = data;
    return true;
}

void ConnectionEngine::set_mode(MonitoringMode mode) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode_ == mode) return;
        if (policy_.active_kind() == OperationKind::automatic ||
            (mode != MonitoringMode::recover && automatic_campus_verification_operation_)) cancel_locked(false);
        if (mode != MonitoringMode::recover) invalidate_campus_service_locked();
        mode_ = mode;
        session_audit_after_.reset();
        consecutive_partial_ = 0;
        consecutive_transport_passes_ = 0;
        policy_.automatic_enabled = mode != MonitoringMode::manual;
        emit(copy::format("event.modeChanged", {{"mode", std::string(copy::text("mode." + std::string(to_string(mode)) + ".title"))}}));
    }
    flush();
}

void ConnectionEngine::set_area(CampusArea area) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (area_ == area) return;
        if (policy_.busy()) cancel_locked(false);
        invalidate_campus_service_locked();
        area_ = area;
        session_audit_after_.reset();
        consecutive_partial_ = 0;
        consecutive_transport_passes_ = 0;
        last_discovery_.reset();
        last_routing_.reset();
        conflicting_campus_hints_ = false;
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::set_probe_mode(ProbeMode mode) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (probe_mode_ != mode) {
            consecutive_partial_ = 0;
            consecutive_transport_passes_ = 0;
            session_audit_after_.reset();
        }
        probe_mode_ = mode;
        if (mode == ProbeMode::quick) full_check_pending_ = false;
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::reset_partial_confirmation() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.busy()) cancel_locked(false);
        consecutive_partial_ = 0;
        consecutive_transport_passes_ = 0;
        offer_forced_login_ = false;
        last_report_.reset();
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::set_draft(const Credentials& draft) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        draft_ = draft;
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::revert_draft() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        draft_ = saved_.value_or(Credentials{});
        pending_change_ = true;
    }
    flush();
}

ConnectionEngine::SaveResult ConnectionEngine::save_draft() {
    SaveResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Credentials candidate{trim(draft_.account), draft_.password};
        if (read_only_ || !candidate.complete()) {
            result = SaveResult::invalid;
        } else if (saved_ && *saved_ == candidate) {
            draft_ = candidate;
            result = SaveResult::unchanged;
        } else if (hooks_.persist_credentials && !hooks_.persist_credentials(candidate)) {
            result = SaveResult::failed;
        } else {
            saved_ = candidate;
            draft_ = candidate;
            generation_ += 1;
            policy_.credentials_changed(generation_);
            reset_partial_episode_locked();
            emit(copy::format("event.saved", {{"store", std::string(copy::text("app.store"))}}));
            result = SaveResult::saved;
        }
        pending_change_ = true;
    }
    flush();
    return result;
}

bool ConnectionEngine::delete_account() {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) return false;
        cancel_locked(false);
        draft_ = {};
        removed = hooks_.erase_credentials && hooks_.erase_credentials();
        if (removed) {
            saved_.reset();
            ++generation_;
            policy_.credentials_changed(generation_);
            reset_partial_episode_locked();
            offer_forced_login_ = false;
            status_ = online_ ? EngineStatus::online : EngineStatus::needsAccount;
            emit(std::string(copy::text("account.deleted")));
        } else {
            policy_.pause_by_user();
            status_ = EngineStatus::paused;
            emit(std::string(copy::text("account.deleteFailed")));
        }
        pending_change_ = true;
    }
    flush();
    return removed;
}

void ConnectionEngine::pause_auto_login() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.active_kind() == OperationKind::automatic) cancel_locked(false);
        policy_.pause_by_user();
        emit(std::string(copy::text("event.paused")));
    }
    flush();
}

void ConnectionEngine::resume_auto_login() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) return;
        recovery_write_blocked_ = false;
        recovery_submission_pending_ = false;
        policy_.resume();
        emit(std::string(copy::text("event.resumed")));
    }
    flush();
}

bool ConnectionEngine::trust_current_network() {
    bool trusted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!read_only_ && policy_.available && identity_ && link_active_ && !sleeping_ &&
            (!requires_physical_binding_ || authentication_route_)) {
            policy_.authorize();
            trusted = policy_.authorized();
            if (trusted) emit(std::string(copy::text("event.trusted")));
        }
    }
    flush();
    return trusted;
}

void ConnectionEngine::revoke_trust() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.busy()) cancel_locked(true);
        invalidate_campus_service_locked();
        if (policy_.authorized()) {
            policy_.revoke_authorization();
            emit(std::string(copy::text("event.trustRevoked")));
        }
    }
    flush();
}

void ConnectionEngine::reset_partial_episode_locked() {
    consecutive_partial_ = 0;
    consecutive_transport_passes_ = 0;
    partial_attempt_used_ = false;
}

void ConnectionEngine::new_epoch_locked() {
    rapid_recovery_until_.reset();
    session_audit_after_.reset();
    full_check_pending_ = false;
    preserve_pending_submission_locked();
    reset_partial_episode_locked();
    epoch_ += 1;
    remembering_operation_.reset();
    campus_verification_operation_.reset();
    invalidate_campus_service_locked();
    credential_operation_.reset();
    conflicting_campus_hints_ = false;
    policy_.reset_for_path_change();
    policy_.path_changed(epoch_);
    last_discovery_.reset();
    last_routing_.reset();
    last_report_.reset();
    online_ = false;
    offer_forced_login_ = false;
}

void ConnectionEngine::receive_path(const PathSnapshot& incoming) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PathSnapshot path = incoming;
        const auto previous_route = authentication_route_;
        std::set<std::string> physical_interfaces;
        for (const auto& candidate : incoming.physical_candidates) physical_interfaces.insert(candidate.interface_key);
        multiple_physical_ = physical_interfaces.size() > 1 || incoming.descriptors.size() > 1;
        const bool binding_requirement_changed = requires_physical_binding_ != incoming.requires_physical_binding;
        requires_physical_binding_ = incoming.requires_physical_binding;
        system_interfaces_ = incoming.interfaces;
        if (requires_physical_binding_) {
            std::vector<physical::Route> candidates;
            for (const auto& route : incoming.physical_candidates) {
                if (!route.descriptor.reliable()) continue;
                const auto same = std::count_if(incoming.physical_candidates.begin(), incoming.physical_candidates.end(),
                    [&](const physical::Route& other) { return other.interface_key == route.interface_key && !(other == route); });
                if (!same && std::find(candidates.begin(), candidates.end(), route) == candidates.end()) candidates.push_back(route);
            }
            auto known = [&](const physical::Route& route) {
                if (!memory_ok_ || memory_unreadable_) return false;
                const auto match = memory_.match(route.descriptor, hooks_.wall_now(), &suppressed_);
                return match.level == MatchLevel::medium || match.level == MatchLevel::strong;
            };
            std::stable_sort(candidates.begin(), candidates.end(), [&](const physical::Route& a, const physical::Route& b) {
                // Observe the current physical egress even before it is remembered.
                // A saved secondary attachment cannot stand in for a new primary;
                // selecting it here grants neither session trust nor recovery eligibility.
                if (a.is_system_primary != b.is_system_primary) return a.is_system_primary;
                if (known(a) != known(b)) return known(a);
                if (a.descriptor.kind != b.descriptor.kind) return a.descriptor.kind == "ethernet";
                return a.interface_key < b.interface_key;
            });
            authentication_route_.reset();
            if (credential_operation_ && policy_.busy() && previous_route) {
                const auto current = std::find(candidates.begin(), candidates.end(), *previous_route);
                if (current != candidates.end()) authentication_route_ = *current;
            }
            if (!authentication_route_) {
                const auto current = std::find_if(candidates.begin(), candidates.end(), [&](const auto& r) { return known(r) || r.is_system_primary; });
                if (current != candidates.end()) authentication_route_ = *current;
                else if (candidates.size() == 1) authentication_route_ = candidates.front();
            }
            path.descriptors.clear();
            if (authentication_route_) {
                path.usable = true; path.link_active = true;
                path.identity = "physical:" + authentication_route_->identity;
                path.descriptors = {authentication_route_->descriptor};
                path.interfaces = authentication_route_->display_name.empty()
                    ? authentication_route_->descriptor.kind : authentication_route_->display_name;
            } else path.identity.reset();
        } else authentication_route_.reset();
        const bool binding_changed = binding_requirement_changed || authentication_route_ != previous_route;
        const bool was_available = policy_.available;
        const bool available = path.usable && !sleeping_;
        const auto identity = path.usable && path.identity && !path.identity->empty() ? path.identity : std::nullopt;
        const auto descriptors = path.usable ? path.descriptors : std::vector<NetworkDescriptor>{};
        const bool descriptors_changed = descriptors_.size() != descriptors.size() ||
            !std::equal(descriptors_.begin(), descriptors_.end(), descriptors.begin(),
                [](const NetworkDescriptor& a, const NetworkDescriptor& b) { return a.kind == b.kind && a.raw == b.raw; });
        // A physical attachment or routing change can precede a new opaque identity.
        // Repeated unknown observations are stable; display-label changes alone do not
        // invalidate a known identity. Every actual path boundary cancels all old work.
        const bool path_changed = identity != identity_ || was_available != available ||
            link_active_ != path.link_active || (tunnel_ != path.tunnel && !authentication_route_) || binding_changed || descriptors_changed ||
            (!identity && !identity_ && interfaces_ != path.interfaces);
        tunnel_ = path.tunnel;
        constrained_ = path.constrained;
        interfaces_ = path.interfaces;
        link_active_ = path.link_active;
        if (path_changed) {
            if (policy_.busy()) token_.cancel();
            new_epoch_locked();
            if (was_available && !path.usable) emit(std::string(copy::text("event.pathLost")));
        }
        identity_ = identity;
        policy_.available = available;
        descriptors_ = descriptors;
        refresh_memory_locked();
        if (path_changed && available && authentication_route_) {
            rapid_recovery_until_ = now() + 45;
            full_check_pending_ = false;
        }
        if (!sleeping_) {
            if (!path.usable) status_ = EngineStatus::noNetwork;
            else if (path_changed || status_ == EngineStatus::noNetwork) status_ = EngineStatus::idle;
        }
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::set_sleeping(bool sleeping) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sleeping_ == sleeping) return;
        sleeping_ = sleeping;
        if (sleeping) {
            if (policy_.busy()) token_.cancel();
            new_epoch_locked();  // sleep is a trust boundary; the wake path is treated as new
            identity_.reset();
            descriptors_.clear();
            memory_level_ = MatchLevel::none; memory_record_.clear(); memory_area_.clear();
            policy_.available = false;
            status_ = EngineStatus::sleeping;
        } else {
            status_ = EngineStatus::idle;
        }
        pending_change_ = true;
    }
    flush();
}

bool ConnectionEngine::connect() {
    bool may;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        may = can_authenticate_locked(OperationKind::manual);
    }
    return run(OperationKind::manual, may, false);
}

bool ConnectionEngine::check() { return run(OperationKind::manual, false, false); }

bool ConnectionEngine::connect_even_if_online() {
    bool may;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        may = can_authenticate_locked(OperationKind::manual);
    }
    return run(OperationKind::manual, may, true);
}

bool ConnectionEngine::automatic_round() {
    bool may;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode_ == MonitoringMode::manual) return false;
        may = can_authenticate_locked(OperationKind::automatic);
    }
    return run(OperationKind::automatic, may, false);
}

void ConnectionEngine::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!policy_.busy()) return;
        cancel_locked(true);
    }
    flush();
}

bool ConnectionEngine::cancel_automatic_probe() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.active_kind() != OperationKind::automatic || status_ != EngineStatus::checking) return false;
        cancel_locked(false);
    }
    flush();
    return true;
}

void ConnectionEngine::preserve_pending_submission_locked() {
    if (recovery_submission_pending_)
        policy_.add(Hold{HoldReason::uncertain, std::nullopt, std::nullopt, std::nullopt, HoldOrigin::global, {}});
}

void ConnectionEngine::cancel_locked(bool mark_cancelled) {
    preserve_pending_submission_locked();
    token_.cancel();
    remembering_operation_.reset();
    campus_verification_operation_.reset();
    invalidate_campus_service_locked();
    credential_operation_.reset();
    policy_.cancel();
    consecutive_partial_ = 0;
    consecutive_transport_passes_ = 0;
    if (status_ == EngineStatus::checking || status_ == EngineStatus::discovering)
        status_ = online_ ? EngineStatus::online : EngineStatus::idle;
    if (mark_cancelled) { status_ = EngineStatus::cancelled; rapid_recovery_until_.reset(); session_audit_after_.reset(); full_check_pending_ = false; }
    pending_change_ = true;
}

bool ConnectionEngine::can_authenticate_locked(OperationKind kind) const {
    if (read_only_) return false;
    if (kind == OperationKind::automatic) return mode_ == MonitoringMode::recover;
    return mode_ != MonitoringMode::observe;
}

bool ConnectionEngine::run(OperationKind kind, bool may_authenticate, bool force) {
    ConnectionPolicy::Operation op;
    CancelToken token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (kind == OperationKind::manual && policy_.active_kind() == OperationKind::automatic) {
            if (status_ != EngineStatus::checking) return false;
            cancel_locked(false); // Only preempt a credential-free probe, never a submission.
        }
        const auto begun = policy_.begin(kind);
        if (!begun) return false;
        op = *begun;
        token_ = token;
        conflicting_campus_hints_ = false;
        offer_forced_login_ = false;
        status_ = EngineStatus::checking;
        pending_change_ = true;
    }
    flush();
    try {
        perform(op, token, may_authenticate, force);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (policy_.owns(op)) {
                if (credential_operation_ == op.id && recovery_submission_pending_) {
                    preserve_pending_submission_locked();
                    status_ = EngineStatus::uncertain;
                } else {
                    status_ = online_ ? EngineStatus::online : EngineStatus::unavailable;
                    if (kind == OperationKind::automatic) policy_.failed();
                }
                if (automatic_campus_verification_operation_ == op.id) {
                    campus_verification_failed_ = true;
                    confirmed_campus_service_.reset();
                }
                pending_change_ = true;
            }
        }
        token.cancel();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.finish(op)) pending_change_ = true;
        if (credential_operation_ == op.id) credential_operation_.reset();
        if (automatic_campus_verification_operation_ == op.id) {
            automatic_campus_verification_operation_.reset();
            if (status_ == EngineStatus::discovering) status_ = online_ ? EngineStatus::online : EngineStatus::idle;
            pending_change_ = true;
        }
    }
    flush();
    return true;
}

void ConnectionEngine::accept_report_locked(const ConnectivityReport& report) {
    last_report_ = report;
    // A new candidate observation must not inherit a previous service-only
    // confirmation. Authentication still obtains its own fresh discovery below.
    if (physical_hint(report.apple) || physical_hint(report.microsoft)) confirmed_campus_service_.reset();
    if (report.transport_recovered()) {
        consecutive_transport_passes_ = std::min(2, consecutive_transport_passes_ + 1);
        if (consecutive_transport_passes_ >= 2) partial_attempt_used_ = false;
    } else consecutive_transport_passes_ = 0;
    pending_change_ = true;
}

void ConnectionEngine::set_online_locked(bool settle_uncertain) {
    if (last_report_ && last_report_->complete_base_passed()) {
        last_routing_.reset();
        conflicting_campus_hints_ = false;
    }
    if (settle_uncertain) {
        recovery_submission_pending_ = false;
        policy_.verify_accepted_scope(authentication_scope_locked(), now());
    }
    if (!online_) emit(std::string(copy::text("event.online")));
    online_ = true;
    if (settle_uncertain && policy_.settle_uncertain()) emit(std::string(copy::text("event.uncertainSettled")));
    policy_.reset_failures();
    status_ = EngineStatus::online;
}

std::optional<std::uint64_t> ConnectionEngine::generation_for(const Credentials& credentials) const {
    if (saved_ && *saved_ == credentials) return generation_;
    return std::nullopt;
}

void ConnectionEngine::perform(const ConnectionPolicy::Operation& op, const CancelToken& token, bool may_authenticate, bool force) {
    const OperationKind kind = op.kind;
    CampusArea area;
    std::optional<Credentials> credentials;
    ProbeMode probe_mode;
    std::uint64_t memory_gen;
    std::optional<physical::Route> expected_route;
    bool bound = false, prioritized = false, awaiting_submission_proof = false, prefer_teaching = false, prefer_dormitory = false, session_audit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memory_gen = memory_generation_;
        refresh_memory_locked();
        area = area_;
        awaiting_submission_proof = recovery_submission_pending_ ||
            std::any_of(policy_.holds().begin(), policy_.holds().end(), [](const Hold& hold) {
                return hold.reason == HoldReason::uncertain;
            });
        // Keep the saved quick preference; use complete only for this read-only verification.
        bound = requires_physical_binding_ && authentication_route_.has_value();
        expected_route = authentication_route_;
        if (requires_physical_binding_ && !bound) { status_ = EngineStatus::unconfirmed; return; }
        if (kind == OperationKind::automatic && full_check_pending_ && probe_mode_ == ProbeMode::complete) may_authenticate = false;
        prioritized = kind == OperationKind::automatic && !awaiting_submission_proof && can_prioritize_recovery_locked();
        session_audit = kind == OperationKind::automatic && may_authenticate && bound && saved_ &&
            probe_mode_ == ProbeMode::quick && !paused_locked() &&
            (memory_level_ == MatchLevel::medium || memory_level_ == MatchLevel::strong || policy_.authorized()) &&
            session_audit_after_ && now() >= *session_audit_after_;
        probe_mode = awaiting_submission_proof || session_audit ? ProbeMode::complete : prioritized ? ProbeMode::quick : probe_mode_;
        prefer_teaching = prefers_teaching_locked();
        prefer_dormitory = prefers_dormitory_locked();
        if (kind == OperationKind::automatic) credentials = saved_;
        else if (Credentials d{trim(draft_.account), draft_.password}; d.complete()) credentials = d;
    }

    if (!service_.set_expected_physical_route(expected_route)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.owns(op)) { status_ = EngineStatus::unconfirmed; pending_change_ = true; }
        return;
    }
    if (token.cancelled()) return;
    const ConnectivityReport report = service_.probe_with_scope(probe_mode, token,
        kind == OperationKind::automatic && may_authenticate && bound, (awaiting_submission_proof || session_audit) && bound);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!policy_.owns(op) || token.cancelled() || expected_route != authentication_route_) return;
    if (probe_mode == ProbeMode::complete) full_check_pending_ = false;
    if (report.online() && rapid_recovery_until_) {
        rapid_recovery_until_.reset();
        full_check_pending_ = prioritized && probe_mode_ == ProbeMode::complete;
    }
    if (kind == OperationKind::automatic && report.websites_deferred && report.complete_base_passed() && probe_mode_ == ProbeMode::complete)
        full_check_pending_ = true;
    accept_report_locked(report);
    lock.unlock();
    if (hooks_.report) hooks_.report(report);
    lock.lock();
    if (!policy_.owns(op)) return;

    const bool campus_redirect = bound && (physical_hint(report.apple) || physical_hint(report.microsoft));
    const bool authentication_required = bound &&
        ((physical_hint(report.apple) && report.apple.authentication_required) ||
         (physical_hint(report.microsoft) && report.microsoft.authentication_required));
    // A public endpoint may remain reachable after the campus session expires.
    // Periodic base-only audits also observe the peer that quick checks cancel.
    if (kind == OperationKind::automatic && may_authenticate && bound) {
        if (campus_redirect || !report.online() || (session_audit && !report.complete_base_passed()))
            session_audit_after_ = now();
        else if (!session_audit_after_ || report.complete_base_passed())
            session_audit_after_ = partial_attempt_used_ && consecutive_transport_passes_ == 1
                ? now() : now() + kSessionAuditSeconds;
    }
    const bool partial = kind == OperationKind::automatic && bound
        ? authentication_required : report.recoverable_partial();
    consecutive_partial_ = kind == OperationKind::automatic && partial ? std::min(2, consecutive_partial_ + 1) : 0;
    const bool automatic_partial = kind == OperationKind::automatic && partial &&
        consecutive_partial_ >= 2 && !partial_attempt_used_;
    const bool manual_partial = kind == OperationKind::manual && partial && may_authenticate;
    if (report.online()) {
        if (kind == OperationKind::automatic) policy_.observe_probe(true);
        else policy_.reset_probe_history();
        // A single success in a complete partial report cannot resolve an uncertain submission.
        set_online_locked(bound ? report.complete_base_passed() : report.complete_all_passed());
        if (!(force && kind == OperationKind::manual && may_authenticate) && !automatic_partial && !manual_partial) {
            offer_forced_login_ = kind == OperationKind::manual && may_authenticate && credentials.has_value();
            const auto healthy_endpoint = [&](const EndpointResult& endpoint) {
                return endpoint.succeeded() || (report.mode == ProbeMode::quick && endpoint.state == EndpointState::notCompleted);
            };
            const bool healthy_base = healthy_endpoint(report.apple) && healthy_endpoint(report.microsoft) &&
                !report.campus_recovery_evidence.value_or(false) && !report.campus_recoverable_partial();
            if (healthy_base) {
                lock.unlock();
                confirm_remembered_campus_service(op, token, report, memory_gen);
            }
            return;
        }
    } else {
        if (kind == OperationKind::automatic) policy_.observe_probe(false);
        online_ = false;
        status_ = EngineStatus::offline;
        if (kind == OperationKind::manual && !may_authenticate) {
            emit(copy::format("event.manualCheckFailed",
                              {{"reason", std::string(copy::text("diagnosis." + std::string(to_string(report.diagnosis())) + ".title"))}}));
        }
    }

    if (!may_authenticate) return;
    if (!credentials) { status_ = EngineStatus::needsAccount; return; }
    const auto gen = generation_for(*credentials);
    if (kind == OperationKind::automatic) {
        if (paused_locked()) { status_ = EngineStatus::paused; return; }
        if (cooldown_remaining_locked().has_value()) { status_ = EngineStatus::coolingDown; return; }
        if (!automatic_partial && policy_.consecutive_probe_failures() < 2) { status_ = EngineStatus::confirming; return; }
    }

    // Adapter enumeration does not establish which physical egress carries the portal.
    // A multi-attachment snapshot can display both, but cannot admit automatic credentials.
    if (kind == OperationKind::automatic && ((requires_physical_binding_ &&
        (!authentication_route_ || ((memory_level_ != MatchLevel::medium && memory_level_ != MatchLevel::strong) && !policy_.authorized()))) ||
        descriptors_.size() > 1)) {
        // A selected attachment still needs the user's trust choice; ambiguous or
        // missing attachments cannot claim that trusting one path is sufficient.
        status_ = authentication_route_ && descriptors_.size() == 1 ? EngineStatus::needsTrust : EngineStatus::unconfirmed;
        return;
    }
    CampusDiscovery discovery{Discovery::confirmed, Discovery::confirmed};
    const bool needs_discovery = kind == OperationKind::automatic || area == CampusArea::automatic;
    if (needs_discovery) {
        start_discovery_locked();
        status_ = EngineStatus::discovering;
        pending_change_ = true;
        lock.unlock();
        flush();
        discovery = service_.discover(discovery_scope(area, report, bound, prefer_teaching), token);
        lock.lock();
        if (!policy_.owns(op) || token.cancelled() || expected_route != authentication_route_ || memory_gen != memory_generation_) return;
        last_discovery_ = discovery;
        refresh_memory_locked();
        if (memory_verdict(dormitory_memory_level_, discovery, memory_ambiguous_locked()) == MemoryVerdict::authorized)
            touch_memory_locked(discovery.dormitory == Discovery::confirmed ? CampusArea::dormitory : CampusArea::teaching);
    }

    prefer_teaching = prefers_teaching_locked();
    prefer_dormitory = prefers_dormitory_locked();
    const auto routing = resolve_campus_routing(area, report, discovery, bound, prefer_teaching, prefer_dormitory);
    record_routing_locked(area, report, discovery, routing);
    if (routing.decision != RoutingDecision::selected) {
        conflicting_campus_hints_ = routing.decision == RoutingDecision::conflictingHints;
        status_ = EngineStatus::unconfirmed;
        pending_change_ = true;
        return;
    }
    if (needs_discovery && verified_scope_locked(area, report, discovery, bound, prefer_teaching, prefer_dormitory)) {
        confirmed_campus_service_ = CampusServiceConfirmation{campus_service_context_locked(), now() + kCampusServiceRefreshSeconds};
        campus_service_retry_.reset();
    }
    const Plan decision = plan(kind, routing.choice, discovery, policy_.authorized(), policy_, gen, epoch_, dormitory_memory_level_, memory_ambiguous_locked());
    switch (decision.decision) {
        case PlanDecision::unconfirmed: status_ = EngineStatus::unconfirmed; if (kind == OperationKind::automatic) policy_.failed(); return;
        case PlanDecision::needsTrust: status_ = EngineStatus::needsTrust; return;
        case PlanDecision::paused: status_ = EngineStatus::paused; return;
        case PlanDecision::submit: break;
    }

    // A forget / revoke while this round was in flight must not be overtaken by its stale plan.
    if (memory_gen != memory_generation_) { status_ = EngineStatus::cancelled; return; }
    CampusArea target = decision.target;
    const double reservation_started = now();
    const auto previous_cooldown = policy_.cooldown_state();
    const auto previous_deadline_wall = recovery_deadline_wall_;
    const bool previous_submission_pending = recovery_submission_pending_;
    policy_.attempt_started(reservation_started);
    recovery_deadline_wall_ = hooks_.wall_now() + 90;
    recovery_submission_pending_ = true;
    if (!persist_recovery_locked()) { status_ = EngineStatus::security; return; }
    // Every login round spends the sustained-partial budget, including explicit/offline
    // rounds and preflight failures. The existing no-submission fallback stays in this
    // round; it can call attempt twice but can submit credentials at most once.
    partial_attempt_used_ = true;
    consecutive_transport_passes_ = 0;
    status_ = EngineStatus::loggingIn;
    credential_operation_ = op.id;
    pending_change_ = true;
    lock.unlock();
    flush();
    LoginAttempt attempt = service_.attempt_with_requirements(*credentials, target, token, routing.require_client_config);
    lock.lock();
    if (!policy_.owns(op)) return;

    if ((attempt.outcome == Outcome::unavailable || attempt.outcome == Outcome::challengeRefused) &&
        !attempt.credential_submission_started && target == CampusArea::teaching && area == CampusArea::automatic && routing.allow_unsent_fallback) {
        lock.unlock();
        const CampusDiscovery again = service_.discover(CampusArea::dormitory, token);
        lock.lock();
        if (!policy_.owns(op)) return;
        const bool manual = kind == OperationKind::manual;
        const bool again_remembered = memory_verdict(dormitory_memory_level_, again, memory_ambiguous_locked()) == MemoryVerdict::authorized;
        if (memory_gen == memory_generation_ &&
            ((again.teaching == Discovery::confirmed && again.dormitory == Discovery::confirmed) || again_remembered) &&
            (manual || policy_.authorized() || again_remembered) &&
            (manual || policy_.blocking(CampusArea::dormitory, gen, epoch_) == nullptr)) {
            target = CampusArea::dormitory;
            lock.unlock();
            attempt = service_.attempt(*credentials, target, token);
            lock.lock();
            if (!policy_.owns(op)) return;
        }
    }

    // The service contract reserves these two outcomes for failures before credentials
    // were sent. Release this reservation only; an earlier submission keeps its exact
    // remaining deadline. Cancelled/uncertain results must retain the protection.
    if (!attempt.credential_submission_started &&
        (attempt.outcome == Outcome::unavailable || attempt.outcome == Outcome::challengeRefused)) {
        policy_.restore_cooldown_state(previous_cooldown);
        recovery_deadline_wall_ = previous_deadline_wall;
        recovery_submission_pending_ = previous_submission_pending;
    } else if (attempt.outcome != Outcome::cancelled) {
        recovery_submission_pending_ = false;
    }
    if (attempt.outcome != Outcome::unavailable && attempt.outcome != Outcome::challengeRefused) rapid_recovery_until_.reset();
    if (attempt.outcome == Outcome::success) {
        recovery_deadline_wall_ = hooks_.wall_now() + 90;
        full_check_pending_ = bound && probe_mode_ == ProbeMode::complete;
    }
    handle_attempt_locked(kind, target, gen, attempt);
    if (attempt.outcome != Outcome::success) return;

    // Read-only verification: at most three probes, never another submission.
    const double delays[] = {0, 2, 5};
    for (const double delay : delays) {
        status_ = EngineStatus::verifying;
        pending_change_ = true;
        lock.unlock();
        flush();
        if (delay > 0) {
            if (hooks_.sleep_cancellable) hooks_.sleep_cancellable(delay, token);
            else hooks_.sleep(delay);
        }
        if (token.cancelled()) return;
        const ConnectivityReport verify = service_.probe_with_scope(bound ? ProbeMode::complete : probe_mode, token, false, bound);
        lock.lock();
        if (!policy_.owns(op)) return;
        accept_report_locked(verify);
        if (verify.online()) set_online_locked(bound ? verify.complete_base_passed() : verify.complete_all_passed());
        else online_ = false;
        lock.unlock();
        if (hooks_.report) hooks_.report(verify);
        lock.lock();
        if (!policy_.owns(op)) return;
        if (verify.online() && ((bound && verify.complete_base_passed()) || !verify.complete_partial())) return;
    }
    if (last_report_ && last_report_->complete_partial()) {
        status_ = EngineStatus::online;
        return;
    }
    status_ = EngineStatus::accepted;
    emit(std::string(copy::text("event.accepted")));
}

void ConnectionEngine::handle_attempt_locked(OperationKind kind, CampusArea target, std::optional<std::uint64_t> gen,
                                             const LoginAttempt& attempt) {
    last_attempt_ = attempt;
    pending_change_ = true;
    if (attempt.outcome == Outcome::success) {
        last_success_area_ = target;
        const auto released = policy_.record_success(target, gen, epoch_);
        policy_.accepted(now(), authentication_scope_locked());
        policy_.reset_failures();
        emit(std::string(copy::text(target == CampusArea::dormitory ? "event.succeededDormitory" : "event.succeededTeaching")));
        if (!released.empty()) emit(copy::format("event.holdReleased", {{"area", area_text(target)}}));
        if (kind == OperationKind::manual && identity_ && link_active_) policy_.authorize();
        touch_memory_locked(target, true);
        return;
    }
    if (attempt.outcome == Outcome::cancelled) {
        preserve_pending_submission_locked();
        status_ = EngineStatus::cancelled;
        return;
    }
    const auto hold = policy_.record_failure(area_ == CampusArea::automatic, target, gen, epoch_, attempt);
    switch (attempt.outcome) {
        case Outcome::rejected:
        case Outcome::invalidCredentials: status_ = EngineStatus::rejected; break;
        case Outcome::uncertain: status_ = EngineStatus::uncertain; break;
        case Outcome::security: status_ = EngineStatus::security; break;
        default: status_ = EngineStatus::unavailable; break;
    }
    const std::string reason_key = attempt.error != PortalError::none
                                       ? "portalError." + std::string(to_string(attempt.error))
                                       : (attempt.outcome == Outcome::rejected ? std::string("portalError.rejected")
                                                                               : "portalError." + std::string(to_string(attempt.outcome)));
    emit(copy::format("event.failed", {{"reason", std::string(copy::text(reason_key))}}));
    if (hold && hold->reason == HoldReason::credentials && hold->area) {
        emit(copy::format("event.holdCreated",
                          {{"area", area_text(hold->area)},
                           {"scope", std::string(copy::text(hold->sticky() ? "hold.scope.sticky" : "hold.scope.network"))}}));
    }
    if (kind == OperationKind::automatic) policy_.failed();
}

bool ConnectionEngine::paused_locked() const {
    if (mode_ != MonitoringMode::recover || !saved_) return false;
    const std::optional<std::uint64_t> gen = generation_;
    if (policy_.global_hold(gen, epoch_)) return true;
    if (area_ == CampusArea::automatic)
        return policy_.blocking(CampusArea::teaching, gen, epoch_) && policy_.blocking(CampusArea::dormitory, gen, epoch_);
    return policy_.blocking(area_, gen, epoch_) != nullptr;
}

std::optional<HoldReason> ConnectionEngine::pause_reason_locked() const {
    if (!paused_locked()) return std::nullopt;
    const std::optional<std::uint64_t> gen = generation_;
    if (const Hold* h = policy_.global_hold(gen, epoch_)) return h->reason;
    const CampusArea probe = area_ == CampusArea::automatic ? CampusArea::teaching : area_;
    if (const Hold* h = policy_.blocking(probe, gen, epoch_)) return h->reason;
    return HoldReason::credentials;
}

AutoLoginState ConnectionEngine::auto_login_locked() const {
    if (mode_ != MonitoringMode::recover) return AutoLoginState::off;
    if (!saved_) return AutoLoginState::needsAccount;
    if (paused_locked()) return AutoLoginState::paused;
    if (descriptors_.size() > 1) return AutoLoginState::awaitingProof;
    if (!policy_.authorized() && requires_physical_binding_ && authentication_route_ &&
        memory_level_ != MatchLevel::medium && memory_level_ != MatchLevel::strong) return AutoLoginState::needsTrust;
    if (!policy_.authorized() && needs_dormitory_eligibility_locked()) {
        switch (memory_verdict(dormitory_memory_level_, last_discovery_.value_or(CampusDiscovery{}), memory_ambiguous_locked())) {
            case MemoryVerdict::authorized: break;
            case MemoryVerdict::awaitingProof:
                if (online_ && !last_discovery_) break;  // presentation only; a future login still requires fresh proof
                return AutoLoginState::awaitingProof;
            default: return AutoLoginState::needsTrust;
        }
    }
    if (!online_ && cooldown_remaining_locked().has_value()) return AutoLoginState::coolingDown;
    if (area_ == CampusArea::automatic) {
        for (const Hold& h : policy_.holds())
            if (h.area && h.generation == std::optional<std::uint64_t>(generation_)) return AutoLoginState::limited;
    }
    return AutoLoginState::ready;
}

std::optional<CampusArea> ConnectionEngine::remembered_entry_locked() const {
    if (!memory_ok_ || !requires_physical_binding_ || !authentication_route_ || descriptors_.size() != 1) return std::nullopt;
    const auto match = memory_.match(authentication_route_->descriptor, hooks_.wall_now(), &suppressed_);
    if (match.level != MatchLevel::medium && match.level != MatchLevel::strong) return std::nullopt;
    const auto record = std::find_if(memory_.records().begin(), memory_.records().end(), [&](const MemoryRecord& value) {
        return value.id == match.record;
    });
    if (record == memory_.records().end() || !record->proof_teaching) return std::nullopt;
    if (record->area == "teaching") return CampusArea::teaching;
    if (record->area == "dormitory" && record->proof_dormitory) return CampusArea::dormitory;
    return std::nullopt;
}
bool ConnectionEngine::prefers_teaching_locked() const { return remembered_entry_locked() == CampusArea::teaching; }
bool ConnectionEngine::prefers_dormitory_locked() const { return remembered_entry_locked() == CampusArea::dormitory; }

ConnectionEngine::CampusServiceContext ConnectionEngine::campus_service_context_locked() const {
    return {epoch_, memory_generation_, area_, authentication_route_};
}

bool ConnectionEngine::automatic_campus_verification_eligible_locked() const {
    return mode_ == MonitoringMode::recover && !read_only_ && !sleeping_ && policy_.available && policy_.foreground &&
        remember_blocker_locked().empty() && (memory_level_ == MatchLevel::medium || memory_level_ == MatchLevel::strong);
}

void ConnectionEngine::invalidate_campus_service_locked() {
    automatic_campus_verification_operation_.reset();
    confirmed_campus_service_.reset();
    campus_service_retry_.reset();
    campus_verification_failed_ = false;
    last_discovery_.reset();
    last_routing_.reset();
    conflicting_campus_hints_ = false;
    pending_change_ = true;
}

void ConnectionEngine::confirm_remembered_campus_service(const ConnectionPolicy::Operation& op, const CancelToken& token,
                                                       const ConnectivityReport& report, std::uint64_t memory_generation) {
    CampusServiceContext context;
    CampusArea discovery_area;
    bool prefer_teaching = false, prefer_dormitory = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!policy_.owns(op) || token.cancelled() || memory_generation != memory_generation_ ||
            !automatic_campus_verification_eligible_locked()) return;
        context = campus_service_context_locked();
        if (confirmed_campus_service_ && confirmed_campus_service_->context == context &&
            now() < confirmed_campus_service_->refresh_after && required_campus_verification_locked() == Discovery::confirmed) return;
        if (campus_service_retry_ && campus_service_retry_->context == context && now() < campus_service_retry_->after) return;
        // This phase keeps the current owner and does not reserve or submit authentication.
        automatic_campus_verification_operation_ = op.id;
        confirmed_campus_service_.reset();
        campus_service_retry_ = CampusServiceRetry{context, now() + 30};
        start_discovery_locked();
        status_ = EngineStatus::discovering;
        prefer_teaching = prefers_teaching_locked();
        prefer_dormitory = prefers_dormitory_locked();
        discovery_area = discovery_scope(context.area, report, context.route.has_value(), prefer_teaching);
    }
    flush();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (automatic_campus_verification_operation_ != op.id || !policy_.owns(op) || token.cancelled() ||
            context != campus_service_context_locked() || !automatic_campus_verification_eligible_locked()) return;
    }
    const auto discovery = service_.discover(discovery_area, token);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (automatic_campus_verification_operation_ != op.id || !policy_.owns(op) || token.cancelled() ||
            context != campus_service_context_locked() || !automatic_campus_verification_eligible_locked()) return;
        last_discovery_ = discovery;
        record_routing_locked(context.area, report, discovery,
            resolve_campus_routing(context.area, report, discovery, context.route.has_value(), prefer_teaching, prefer_dormitory));
        const bool confirmed = verified_scope_locked(context.area, report, discovery, context.route.has_value(), prefer_teaching, prefer_dormitory).has_value();
        if (confirmed) {
            // Refresh only during an ordinary healthy check. This timestamp grants no
            // authentication authority and never changes saved proof or its expiry.
            confirmed_campus_service_ = CampusServiceConfirmation{context, now() + kCampusServiceRefreshSeconds};
            campus_service_retry_.reset();
        } else {
            confirmed_campus_service_.reset();
            // Retry eligibility starts at completion. The controller retains its original cadence.
            campus_service_retry_ = CampusServiceRetry{context, now() + 30};
        }
        campus_verification_failed_ = !confirmed;
        automatic_campus_verification_operation_.reset();
        status_ = online_ ? EngineStatus::online : EngineStatus::idle;
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::start_discovery_locked() {
    ++discovery_revision_;
    confirmed_campus_service_.reset();
    last_discovery_.reset();
    last_routing_.reset();
    campus_verification_failed_ = false;
    conflicting_campus_hints_ = false;
    pending_change_ = true;
}

void ConnectionEngine::record_routing_locked(CampusArea choice, const ConnectivityReport& report,
                                            const CampusDiscovery& discovery, const CampusRoutingResolution& routing) {
    last_routing_ = routing;
    routing_area_ = choice;
    routing_epoch_ = epoch_;
    routing_discovery_ = discovery;
    routing_apple_hint_ = physical_hint(report.apple);
    routing_microsoft_hint_ = physical_hint(report.microsoft);
}

std::optional<CampusRoutingResolution> ConnectionEngine::current_routing_locked() const {
    if (!last_discovery_) return std::nullopt;
    const auto report = last_report_.value_or(ConnectivityReport{});
    if (last_routing_ && routing_epoch_ == epoch_ && routing_area_ == area_ && routing_discovery_ == last_discovery_ &&
        routing_apple_hint_ == physical_hint(report.apple) && routing_microsoft_hint_ == physical_hint(report.microsoft)) return last_routing_;
    return resolve_campus_routing(area_, report, *last_discovery_, requires_physical_binding_ && authentication_route_.has_value(),
        prefers_teaching_locked(), prefers_dormitory_locked());
}

bool ConnectionEngine::needs_dormitory_eligibility_locked() const {
    if (area_ != CampusArea::automatic) return area_ == CampusArea::dormitory;
    if (const auto routing = current_routing_locked()) {
        if (routing->decision == RoutingDecision::selected && routing->choice != CampusArea::automatic)
            return routing->choice == CampusArea::dormitory;
        if (routing->decision == RoutingDecision::conflictingHints || routing->decision == RoutingDecision::needsSelection) return false;
        if (routing->decision == RoutingDecision::unconfirmed) {
            const auto apple = last_report_ ? physical_hint(last_report_->apple) : std::nullopt;
            const auto microsoft = last_report_ ? physical_hint(last_report_->microsoft) : std::nullopt;
            if (apple && microsoft && apple != microsoft) return false;
            return (apple ? apple : microsoft) == CampusPortalHint::dormitory;
        }
    }
    return (last_discovery_ && last_discovery_->dormitory == Discovery::confirmed) ||
        dormitory_memory_level_ == MatchLevel::medium || dormitory_memory_level_ == MatchLevel::strong;
}

bool ConnectionEngine::needs_memory_update_locked() const {
    if ((memory_level_ != MatchLevel::medium && memory_level_ != MatchLevel::strong) || !needs_dormitory_eligibility_locked()) return false;
    const auto verdict = memory_verdict(dormitory_memory_level_, last_discovery_.value_or(CampusDiscovery{}), memory_ambiguous_locked());
    return verdict == MemoryVerdict::notRemembered || verdict == MemoryVerdict::conflict;
}

Discovery ConnectionEngine::required_campus_verification_locked() const {
    if (campus_verification_failed_) return Discovery::unconfirmed;
    if (!policy_.available || sleeping_ || !last_discovery_) return Discovery::notChecked;
    // A completed independent check used its own current hints. The published
    // connectivity report remains unchanged and may contain an older redirect.
    if (confirmed_campus_service_ && confirmed_campus_service_->context == campus_service_context_locked()) return Discovery::confirmed;
    if (const auto routing = current_routing_locked(); routing && routing->decision != RoutingDecision::selected) return Discovery::unconfirmed;
    if (last_discovery_->teaching == Discovery::confirmed &&
        (!needs_dormitory_eligibility_locked() || last_discovery_->dormitory == Discovery::confirmed)) return Discovery::confirmed;
    return last_discovery_->teaching == Discovery::notChecked && last_discovery_->dormitory == Discovery::notChecked
        ? Discovery::notChecked : Discovery::unconfirmed;
}

std::optional<CampusArea> ConnectionEngine::verified_scope_locked(CampusArea choice, const ConnectivityReport& report,
                                                                 const CampusDiscovery& discovery, bool bound, bool prefer_teaching, bool prefer_dormitory) const {
    const auto routing = resolve_campus_routing(choice, report, discovery, bound, prefer_teaching, prefer_dormitory);
    if (routing.decision != RoutingDecision::selected) return std::nullopt;
    const auto target = routing.choice;
    if (discovery.teaching != Discovery::confirmed) return std::nullopt;
    if (target == CampusArea::teaching || (target == CampusArea::automatic && discovery.teaching_client_bound)) return CampusArea::teaching;
    if (discovery.dormitory == Discovery::confirmed) return CampusArea::dormitory;
    return target == CampusArea::automatic ? std::optional<CampusArea>(CampusArea::teaching) : std::nullopt;
}

EngineSnapshot ConnectionEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    EngineSnapshot s;
    s.status = status_;
    if (last_report_) s.diagnosis = last_report_->diagnosis();
    s.last_report = last_report_;
    s.online = online_;
    s.busy = policy_.busy();
    s.busy_kind = policy_.active_kind();
    s.mode = mode_;
    s.area = area_;
    s.probe_mode = probe_mode_;
    s.read_only = read_only_;
    s.has_saved = saved_.has_value();
    s.draft_complete = Credentials{trim(draft_.account), draft_.password}.complete();
    s.unsaved_changes = saved_ ? !(*saved_ == draft_) : (!draft_.account.empty() || !draft_.password.empty());
    s.saved_account_hint = saved_ ? mask_account(saved_->account) : std::string();
    s.auto_login = auto_login_locked();
    s.paused = paused_locked();
    s.pause_reason = pause_reason_locked();
    s.holds = policy_.holds();
    s.cooldown_remaining = cooldown_remaining_locked();
    s.trusted = policy_.authorized();
    s.multiple_networks = descriptors_.size() > 1;
    s.can_trust = !read_only_ && policy_.available && identity_.has_value() && link_active_ && !policy_.authorized() && !s.multiple_networks
        && (!requires_physical_binding_ || authentication_route_.has_value());
    s.network_available = policy_.available;
    s.sleeping = sleeping_;
    s.tunnel = tunnel_;
    s.constrained = constrained_;
    s.interfaces = interfaces_;
    s.authentication_route = authentication_route_;
    s.system_interfaces = system_interfaces_;
    s.offer_forced_login = offer_forced_login_;
    s.last_attempt = last_attempt_;
    s.last_success_area = last_success_area_;
    s.last_discovery = last_discovery_;
    s.next_check_seconds = next_check_delay_locked();
    s.epoch = epoch_;
    if (saved_) s.generation = generation_;
    s.memory_level = memory_level_;
    s.remembered = memory_level_ == MatchLevel::medium || memory_level_ == MatchLevel::strong;
    s.needs_network_memory_update = needs_memory_update_locked();
    s.recovery_setup_has_network = (s.remembered && !s.needs_network_memory_update) ||
        (!requires_physical_binding_ && area_ == CampusArea::teaching);
    s.required_campus_verification = required_campus_verification_locked();
    s.required_campus_confirmed = s.required_campus_verification == Discovery::confirmed;
    const auto routing = current_routing_locked();
    s.needs_selection = routing && routing->decision == RoutingDecision::needsSelection &&
        !(online_ && last_report_ && last_report_->complete_base_passed());
    s.discovery_revision = discovery_revision_;
    s.remember_blocker = remember_blocker_locked();
    s.can_remember = !policy_.busy() && s.remember_blocker.empty();
    s.verifying_memory = remembering_operation_.has_value();
    s.auto_verifying_campus = automatic_campus_verification_operation_.has_value();
    s.verifying_campus = campus_verification_operation_.has_value() || s.auto_verifying_campus;
    s.automatic_campus_verification_failed = automatic_campus_verification_eligible_locked() && campus_verification_failed_ &&
        campus_service_retry_ && campus_service_retry_->context == campus_service_context_locked();
    s.automatic_campus_verification_pending = s.automatic_campus_verification_failed;
    s.can_verify_campus = can_verify_campus_locked();
    s.conflicting_campus_hints = conflicting_campus_hints_;
    s.memory_count = static_cast<std::size_t>(std::count_if(memory_.records().begin(), memory_.records().end(),
        [&](const MemoryRecord& record) { return !suppressed_.count(record.id); }));
    s.memory_unreadable = memory_unreadable_;
    s.memory_write_failed = memory_write_failed_;
    if (!s.multiple_networks && auto_login_locked() == AutoLoginState::awaitingProof) s.proof_wait = tunnel_ ? "tunnel" : "unconfirmed";
    else if (dormitory_memory_level_ == MatchLevel::conflict && auto_login_locked() == AutoLoginState::needsTrust) s.proof_wait = "conflict";
    return s;
}

bool ConnectionEngine::paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_locked();
}

AutoLoginState ConnectionEngine::auto_login_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return auto_login_locked();
}

int ConnectionEngine::next_check_delay() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_check_delay_locked();
}

std::optional<AuthenticationScope> ConnectionEngine::authentication_scope_locked() const {
    if (!requires_physical_binding_ || !authentication_route_ || !authentication_route_->descriptor.reliable()) return std::nullopt;
    AuthenticationScope scope{authentication_route_->descriptor.kind, authentication_route_->interface_key};
    return scope.valid() ? std::optional<AuthenticationScope>(scope) : std::nullopt;
}

std::optional<double> ConnectionEngine::cooldown_remaining_locked() const {
    return policy_.cooldown_remaining(now(), authentication_scope_locked());
}

bool ConnectionEngine::can_prioritize_recovery_locked() const {
    return rapid_recovery_until_ && now() < *rapid_recovery_until_ && mode_ == MonitoringMode::recover && !read_only_ &&
        saved_ && policy_.available && policy_.foreground && !sleeping_ && !paused_locked() &&
        !recovery_submission_pending_ && !cooldown_remaining_locked().has_value();
}

int ConnectionEngine::next_check_delay_locked() const {
    if (mode_ == MonitoringMode::recover && !read_only_ && probe_mode_ == ProbeMode::quick &&
        session_audit_after_ && now() >= *session_audit_after_ && partial_attempt_used_ && consecutive_transport_passes_ == 1 &&
        policy_.available && !sleeping_ && !paused_locked()) return 2;
    if (rapid_recovery_until_ && now() < *rapid_recovery_until_ && status_ == EngineStatus::idle &&
        policy_.available && !sleeping_ && mode_ != MonitoringMode::manual) return 0;
    if (full_check_pending_ && policy_.available && !sleeping_ && probe_mode_ == ProbeMode::complete && !paused_locked() && !recovery_submission_pending_) return 2;
    if (can_prioritize_recovery_locked() && status_ != EngineStatus::needsTrust) {
        if (!last_report_ || status_ == EngineStatus::idle) return 0;
        if (policy_.consecutive_probe_failures() == 1) return 1;
        if (!online_) return 3;
    }
    if (mode_ == MonitoringMode::recover && !read_only_ && consecutive_partial_ == 1 && !partial_attempt_used_ &&
        !paused_locked() && !cooldown_remaining_locked().has_value()) return 5;
    return policy_.next_check_delay(constrained_, now(), paused_locked());
}

Credentials ConnectionEngine::draft() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return draft_;
}

// ---------------- network memory ----------------
void ConnectionEngine::refresh_memory_locked() {
    memory_level_ = MatchLevel::none;
    dormitory_memory_level_ = MatchLevel::none;
    memory_record_.clear();
    dormitory_memory_record_.clear();
    memory_area_.clear();
    if (!memory_ok_ || !policy_.available || sleeping_ || !link_active_ || !identity_ || identity_->empty() || descriptors_.empty() ||
        (requires_physical_binding_ && !authentication_route_)) return;
    const auto m = memory_.match_any(descriptors_, hooks_.wall_now(), &suppressed_);
    memory_level_ = m.level;
    memory_record_ = m.record;
    memory_area_ = m.area;
    const auto dorm = memory_.match_any(descriptors_, hooks_.wall_now(), &suppressed_, true);
    dormitory_memory_level_ = dorm.level;
    dormitory_memory_record_ = dorm.record;
}

bool ConnectionEngine::restore_memory(const std::optional<std::string>& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (remembering_operation_ || campus_verification_operation_ || automatic_campus_verification_operation_) cancel_locked(true);
    invalidate_campus_service_locked();
    ++memory_generation_;
    memory_ok_ = false;
    memory_unreadable_ = false;
    pending_change_ = true;
    if (!hooks_.random_hex || !hooks_.persist_memory) return true;  // memory support is disabled
    if (!text) {
        memory_ = NetworkMemory(hooks_.random_hex());
        memory_ok_ = memory_.has_salt();
    } else if (auto decoded = NetworkMemory::decode(*text, hooks_.wall_now()); decoded && decoded->has_salt()) {
        memory_ = std::move(*decoded);
        memory_ok_ = true;
    } else {
        memory_unreadable_ = true;
        return false;
    }
    refresh_memory_locked();
    return true;
}

std::string ConnectionEngine::remember_blocker_locked() const {
    if (read_only_) return "readOnly";
    if (memory_unreadable_) return "unreadable";
    if (!memory_ok_ || !policy_.available || sleeping_ || !link_active_ || !identity_ || identity_->empty() ||
        (requires_physical_binding_ && !authentication_route_)) return "needsProof";
    if (descriptors_.size() != 1 || !descriptors_.front().reliable()) return descriptors_.empty() ? "needsProof" : "unreliable";
    return "";
}

bool ConnectionEngine::commit_memory_locked(NetworkMemory next, const std::vector<std::string>& ids_to_suppress,
                                             const std::string& freshly_remembered) {
    for (const auto& id : suppressed_)
        if (id != freshly_remembered) next.forget(id);
    if (hooks_.persist_memory && hooks_.persist_memory(next.encode())) {
        memory_ = std::move(next);
        memory_write_failed_ = false;
        return true;
    }
    // The old file is untouched. A revocation still has to hold for this session.
    for (const auto& id : ids_to_suppress) suppressed_.insert(id);
    memory_write_failed_ = true;
    return false;
}

void ConnectionEngine::touch_memory_locked(CampusArea area, bool authenticated) {
    const std::string& record = area == CampusArea::dormitory ? dormitory_memory_record_ : memory_record_;
    if (!memory_ok_ || record.empty()) return;
    const auto& records = memory_.records();
    const auto prior = std::find_if(records.begin(), records.end(), [&](const MemoryRecord& item) { return item.id == record; });
    if (prior == records.end()) return;
    const std::string preferred = authenticated ? (area == CampusArea::dormitory ? "dormitory" : "teaching") : prior->area;
    NetworkMemory next = memory_;
    const bool changed = authenticated && area == CampusArea::teaching && prior->proof_dormitory
        ? next.prefer_authenticated_area(record, preferred) : next.touch(record, hooks_.wall_now(), preferred);
    if (changed) {
        commit_memory_locked(std::move(next), {});
        refresh_memory_locked();
    }
}

ConnectionEngine::RememberResult ConnectionEngine::remember_rejection_locked() const {
    const auto blocker = remember_blocker_locked();
    if (blocker == "readOnly") return RememberResult::readOnly;
    if (blocker == "unreadable") return RememberResult::unreadable;
    if (blocker == "unreliable") return RememberResult::unreliable;
    if (policy_.busy()) return RememberResult::busy;
    return RememberResult::needsProof;
}

std::optional<ConnectionEngine::RememberRequest> ConnectionEngine::prepare_remember() {
    RememberRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_.busy() || !remember_blocker_locked().empty()) return std::nullopt;
        const auto operation = policy_.begin(OperationKind::manual);
        if (!operation) return std::nullopt;
        request.operation_ = *operation;
        request.epoch_ = epoch_;
        request.memory_generation_ = memory_generation_;
        request.area_ = area_;
        request.selection_report_ = last_report_.value_or(ConnectivityReport{});
        request.prefer_teaching_ = prefers_teaching_locked();
        request.prefer_dormitory_ = prefers_dormitory_locked();
        request.route_ = authentication_route_;
        request.merging_record_ids_ = memory_.merge_candidates(descriptors_.front(), hooks_.wall_now());
        token_ = request.token_;
        remembering_operation_ = operation->id;
        confirmed_campus_service_.reset();
        campus_service_retry_.reset();
        start_discovery_locked();
        offer_forced_login_ = false;
        status_ = EngineStatus::discovering;
        pending_change_ = true;
    }
    flush();
    return request;
}

bool ConnectionEngine::owns_remember_locked(const RememberRequest& request) const {
    return remembering_operation_ == request.operation_.id && policy_.owns(request.operation_) &&
        !request.token_.cancelled() && epoch_ == request.epoch_ && memory_generation_ == request.memory_generation_ &&
        area_ == request.area_ && authentication_route_ == request.route_ &&
        remember_blocker_locked().empty();
}

bool ConnectionEngine::can_verify_campus_locked() const {
    return !policy_.busy() && remember_blocker_locked().empty() &&
        (memory_level_ == MatchLevel::medium || memory_level_ == MatchLevel::strong);
}

std::optional<ConnectionEngine::CampusVerificationRequest> ConnectionEngine::prepare_verify_campus() {
    CampusVerificationRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!can_verify_campus_locked()) return std::nullopt;
        const auto operation = policy_.begin(OperationKind::manual);
        if (!operation) return std::nullopt;
        request.operation_ = *operation;
        request.epoch_ = epoch_;
        request.memory_generation_ = memory_generation_;
        request.area_ = area_;
        request.selection_report_ = last_report_.value_or(ConnectivityReport{});
        request.prefer_teaching_ = prefers_teaching_locked();
        request.prefer_dormitory_ = prefers_dormitory_locked();
        request.route_ = authentication_route_;
        request.previous_status_ = status_;
        token_ = request.token_;
        campus_verification_operation_ = operation->id;
        confirmed_campus_service_.reset();
        campus_service_retry_.reset();
        start_discovery_locked();
        status_ = EngineStatus::discovering;
        pending_change_ = true;
    }
    flush();
    return request;
}

bool ConnectionEngine::owns_campus_verification_locked(const CampusVerificationRequest& request) const {
    return campus_verification_operation_ == request.operation_.id && policy_.owns(request.operation_) &&
        !request.token_.cancelled() && epoch_ == request.epoch_ && memory_generation_ == request.memory_generation_ &&
        area_ == request.area_ && authentication_route_ == request.route_ && remember_blocker_locked().empty() &&
        (memory_level_ == MatchLevel::medium || memory_level_ == MatchLevel::strong);
}

ConnectionEngine::CampusVerificationResult ConnectionEngine::verify_campus(const CampusVerificationRequest& request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_campus_verification_locked(request)) return CampusVerificationResult::cancelled;
    }
    const bool bound = service_.set_expected_physical_route(request.route_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_campus_verification_locked(request)) return CampusVerificationResult::cancelled;
    }
    auto selection_report = request.selection_report_;
    if (bound && refresh_selection_hints(request.area_, selection_report)) {
        selection_report = service_.probe_verification_hints(request.token_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_campus_verification_locked(request)) return CampusVerificationResult::cancelled;
    }
    CampusDiscovery discovery;
    if (bound) discovery = service_.discover(discovery_scope(request.area_, selection_report,
        request.route_.has_value(), request.prefer_teaching_), request.token_);
    CampusVerificationResult result = CampusVerificationResult::unavailable;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_campus_verification_locked(request)) return CampusVerificationResult::cancelled;
        if (bound) {
            last_discovery_ = discovery;
            record_routing_locked(request.area_, selection_report, discovery,
                resolve_campus_routing(request.area_, selection_report, discovery,
                    request.route_.has_value(), request.prefer_teaching_, request.prefer_dormitory_));
            result = verified_scope_locked(request.area_, selection_report, discovery,
                request.route_.has_value(), request.prefer_teaching_, request.prefer_dormitory_) ? CampusVerificationResult::confirmed : CampusVerificationResult::needsProof;
        }
        campus_verification_failed_ = result != CampusVerificationResult::confirmed;
        if (result == CampusVerificationResult::confirmed)
            confirmed_campus_service_ = CampusServiceConfirmation{campus_service_context_locked(), now() + kCampusServiceRefreshSeconds};
        // A service check never changes memory age/preference, trust, holds, cooldown or probe history.
        policy_.finish(request.operation_);
        campus_verification_operation_.reset();
        status_ = request.previous_status_;
        pending_change_ = true;
    }
    flush();
    return result;
}

ConnectionEngine::RememberResult ConnectionEngine::trust_and_remember() {
    const auto request = prepare_remember();
    if (request) return trust_and_remember(*request);
    std::lock_guard<std::mutex> lock(mutex_);
    return remember_rejection_locked();
}

ConnectionEngine::RememberResult ConnectionEngine::trust_and_remember(const RememberRequest& request) {
    std::optional<physical::Route> expected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_remember_locked(request)) return RememberResult::cancelled;
        expected = request.route_;
    }
    if (!service_.set_expected_physical_route(expected)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Binding may synchronously observe a changed path or cancellation. Only
            // this request may release its operation or publish its failed proof.
            if (!owns_remember_locked(request)) return RememberResult::cancelled;
            status_ = online_ ? EngineStatus::online : paused_locked() ? EngineStatus::paused : EngineStatus::unconfirmed;
            policy_.finish(request.operation_);
            remembering_operation_.reset();
            pending_change_ = true;
        }
        flush();
        return RememberResult::needsProof;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_remember_locked(request)) return RememberResult::cancelled;
    }
    auto selection_report = request.selection_report_;
    if (refresh_selection_hints(request.area_, selection_report)) {
        selection_report = service_.probe_verification_hints(request.token_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_remember_locked(request)) return RememberResult::cancelled;
    }
    // The selected area is fixed when the operation is reserved. Discovery never reads credentials.
    const auto discovery = service_.discover(request.area_ == CampusArea::teaching ? CampusArea::teaching : CampusArea::automatic,
                                             request.token_);
    RememberResult result = RememberResult::needsProof;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owns_remember_locked(request)) return RememberResult::cancelled;
        last_discovery_ = discovery;
        record_routing_locked(request.area_, selection_report, discovery,
            resolve_campus_routing(request.area_, selection_report, discovery,
                request.route_.has_value(), request.prefer_teaching_, request.prefer_dormitory_));
        const auto scope = verified_scope_locked(request.area_, selection_report, discovery,
            request.route_.has_value(), request.prefer_teaching_, request.prefer_dormitory_);
        if (scope) {
            const bool dormitory_scope = *scope == CampusArea::dormitory;
            NetworkMemory next = memory_;
            const auto id = next.remember(descriptors_.front(), dormitory_scope ? "dormitory" : "teaching",
                                          true, dormitory_scope, hooks_.wall_now(), hooks_.random_hex());
            if (!id) result = RememberResult::unreliable;
            else if (hooks_.prepare_memory_merge && [&] {
                std::vector<std::string> removed;
                for (const auto& candidate : request.merging_record_ids_)
                    if (std::none_of(next.records().begin(), next.records().end(), [&](const MemoryRecord& record) { return record.id == candidate; }))
                        removed.push_back(candidate);
                return !removed.empty() && !hooks_.prepare_memory_merge(*id, removed);
            }()) result = RememberResult::failed;
            else if (!commit_memory_locked(std::move(next), {}, *id)) result = RememberResult::failed;
            else {
                if (dormitory_scope) policy_.authorize();  // teaching memory only selects Wi-Fi; it grants no session trust
                suppressed_.erase(*id);
                refresh_memory_locked();
                confirmed_campus_service_ = CampusServiceConfirmation{campus_service_context_locked(), now() + kCampusServiceRefreshSeconds};
                campus_service_retry_.reset();
                emit(std::string(copy::text("event.remembered")));
                result = RememberResult::remembered;
            }
        }
        // Discovery alone cannot prove Internet connectivity or settle an authentication hold.
        status_ = online_ ? EngineStatus::online : paused_locked() ? EngineStatus::paused : EngineStatus::idle;
        if (result == RememberResult::needsProof && !online_ && !paused_locked()) status_ = EngineStatus::unconfirmed;
        policy_.finish(request.operation_);
        remembering_operation_.reset();
        pending_change_ = true;
    }
    flush();
    return result;
}

void ConnectionEngine::forget_network(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_ || !memory_ok_) return;
        const auto ids = memory_.equivalent_ids(id);
        if (ids.empty()) return;
        const auto current = memory_.matching_ids(descriptors_, hooks_.wall_now());
        const bool affects_current = std::any_of(ids.begin(), ids.end(), [&](const std::string& removed) {
            return std::find(current.begin(), current.end(), removed) != current.end();
        });
        if (affects_current) {
            if (policy_.busy()) cancel_locked(false);
            invalidate_campus_service_locked();
            ++memory_generation_;
            policy_.revoke_authorization();
        }
        NetworkMemory next = memory_;
        for (const auto& removed : ids) next.forget(removed);
        commit_memory_locked(std::move(next), ids);
        refresh_memory_locked();
        emit(std::string(copy::text("event.forgot")));
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::forget_current_network() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) return;
        if (policy_.busy()) cancel_locked(false);
        invalidate_campus_service_locked();
        ++memory_generation_;
        policy_.revoke_authorization();
        if (memory_ok_) {
            NetworkMemory next = memory_;
            const auto ids = memory_.matching_ids(descriptors_, hooks_.wall_now());
            for (const auto& id : ids) next.forget(id);
            if (!ids.empty()) commit_memory_locked(std::move(next), ids);
        }
        refresh_memory_locked();
        emit(std::string(copy::text("event.forgot")));
        pending_change_ = true;
    }
    flush();
}

void ConnectionEngine::forget_all_networks() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) return;
        if (policy_.busy()) cancel_locked(false);
        invalidate_campus_service_locked();
        ++memory_generation_;
        policy_.revoke_authorization();
        if (memory_ok_) {
            std::vector<std::string> ids;
            for (const auto& r : memory_.records()) ids.push_back(r.id);
            NetworkMemory next = memory_;
            next.forget_all();
            if (!ids.empty()) commit_memory_locked(std::move(next), ids);
        }
        refresh_memory_locked();
        emit(std::string(copy::text("event.forgotAll")));
        pending_change_ = true;
    }
    flush();
}

std::vector<std::string> ConnectionEngine::current_remembered_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!memory_ok_ || !policy_.available || sleeping_ || !link_active_ || !identity_ || identity_->empty() ||
        descriptors_.size() != 1 || !descriptors_.front().reliable()) return {};
    auto ids = memory_.matching_ids(descriptors_, hooks_.wall_now());
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) { return suppressed_.count(id) != 0; }), ids.end());
    return ids;
}

std::vector<MemoryRecord> ConnectionEngine::remembered_networks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryRecord> out;
    for (const auto& r : memory_.records())
        if (!suppressed_.count(r.id)) out.push_back(r);
    return out;
}

}  // namespace szu
