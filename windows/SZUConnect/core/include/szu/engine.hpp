// ConnectionEngine: the platform-neutral equivalent of the macOS AppModel connection logic.
// UI threads call configuration methods and snapshot(); one worker thread runs operations,
// which block on the injected ConnectionService. Stale results can never publish: every
// operation owns a policy ticket and a CancelToken, and loses both on cancel/path change.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "szu/netmemory.hpp"
#include "szu/physical.hpp"
#include "szu/policy.hpp"
#include "szu/types.hpp"

namespace szu {

class CancelToken {
public:
    CancelToken();
    void cancel() const;
    bool cancelled() const;
    // Waits for cancellation or the interval to elapse; true means cancelled.
    bool wait_for(double seconds) const;
    // Runs fn on cancel (immediately when already cancelled). Returns an id for remove().
    std::uint64_t on_cancel(std::function<void()> fn) const;
    void remove(std::uint64_t id) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

class ConnectionService {
public:
    virtual ~ConnectionService() = default;
    // Connectivity probe. Never carries credentials.
    virtual ConnectivityReport probe(ProbeMode mode, const CancelToken& token) = 0;
    // Private fixed-probe evidence for an independent service check. Decorators
    // must leave published connectivity and website results unchanged.
    virtual ConnectivityReport probe_verification_hints(const CancelToken& token) {
        return probe(ProbeMode::quick, token);
    }
    // Read-only discovery. Dormitory requires both services; automatic may stop
    // after a fresh exact-source teaching CONFIG. Neither operation carries credentials.
    virtual CampusDiscovery discover(CampusArea area, const CancelToken& token) = 0;
    // Exactly one concrete area (teaching or dormitory), at most one credential submission.
    virtual LoginAttempt attempt(const Credentials& credentials, CampusArea area, const CancelToken& token) = 0;
    // A transport must explicitly support a fresh source-bound CONFIG requirement.
    virtual LoginAttempt attempt_with_requirements(const Credentials& credentials, CampusArea area,
                                                   const CancelToken& token, bool require_client_config) {
        if (!require_client_config) return attempt(credentials, area, token);
        LoginAttempt result;
        result.outcome = Outcome::security;
        result.error = PortalError::protocolConflict;
        result.credential_submission_started = false;
        return result;
    }
    // A bound request may never borrow another interface's remembered authority.
    virtual bool set_expected_physical_route(const std::optional<physical::Route>& route) { return !route; }
    virtual ConnectivityReport probe_with_scope(ProbeMode mode, const CancelToken& token,
                                                bool prioritize_base, bool defer_websites) {
        (void)prioritize_base; (void)defer_websites; return probe(mode, token);
    }
};

enum class EngineStatus {
    idle, checking, discovering, loggingIn, verifying, online, offline, confirming, paused, coolingDown,
    unconfirmed, needsTrust, needsAccount, accepted, rejected, uncertain, security, unavailable, cancelled,
    noNetwork, sleeping
};
enum class AutoLoginState { off, needsAccount, paused, needsTrust, coolingDown, limited, ready, awaitingProof };
std::string_view to_string(EngineStatus value);
std::string_view to_string(AutoLoginState value);

struct PathSnapshot {
    bool usable = false;
    std::optional<std::string> identity;  // opaque network fingerprint; never displayed or exported
    bool link_active = true;              // a physical Wi-Fi/Ethernet link is up
    bool constrained = false;             // metered / low-data
    bool tunnel = false;                  // VPN / TUN evidence
    std::string interfaces;               // user-facing summary without SSID
    // Stable, RAM-only features of every live physical attachment (never displayed, exported or stored raw).
    std::vector<NetworkDescriptor> descriptors;
    bool requires_physical_binding = false;
    std::vector<physical::Route> physical_candidates;
};

struct EngineSnapshot {
    EngineStatus status = EngineStatus::idle;
    std::optional<Diagnosis> diagnosis;
    std::optional<ConnectivityReport> last_report;
    bool online = false;
    bool busy = false;
    std::optional<OperationKind> busy_kind;
    MonitoringMode mode = MonitoringMode::manual;
    CampusArea area = CampusArea::automatic;
    ProbeMode probe_mode = ProbeMode::quick;
    bool read_only = false;
    bool has_saved = false;
    bool draft_complete = false;
    bool unsaved_changes = false;
    std::string saved_account_hint;
    AutoLoginState auto_login = AutoLoginState::off;
    bool paused = false;
    std::optional<HoldReason> pause_reason;
    std::vector<Hold> holds;
    std::optional<double> cooldown_remaining;
    bool trusted = false;
    bool can_trust = false;
    bool multiple_networks = false; // physical route ambiguity; never resolved by granting trust
    bool network_available = false;
    bool sleeping = false;
    bool tunnel = false;
    bool constrained = false;
    std::string interfaces;
    std::optional<physical::Route> authentication_route;
    std::string system_interfaces;
    bool offer_forced_login = false;
    std::optional<LoginAttempt> last_attempt;
    std::optional<CampusArea> last_success_area;
    std::optional<CampusDiscovery> last_discovery;
    int next_check_seconds = 30;
    std::uint64_t epoch = 1;
    std::optional<std::uint64_t> generation;
    // network memory
    MatchLevel memory_level = MatchLevel::none;
    bool remembered = false;            // a record matches this network at medium or strong level
    bool needs_network_memory_update = false;
    bool recovery_setup_has_network = false;
    bool required_campus_confirmed = false;
    Discovery required_campus_verification = Discovery::notChecked;
    bool needs_selection = false;
    std::uint64_t discovery_revision = 0;
    bool can_remember = false;         // an idle, reliable path may start fresh credential-free verification
    bool verifying_memory = false;
    bool verifying_campus = false;
    bool auto_verifying_campus = false;
    bool automatic_campus_verification_failed = false;
    bool automatic_campus_verification_pending = false;
    bool can_verify_campus = false;
    bool conflicting_campus_hints = false;
    std::string remember_blocker;       // "" | needsProof | unreliable | readOnly | unreadable
    std::size_t memory_count = 0;
    bool memory_unreadable = false;
    bool memory_write_failed = false;
    std::string proof_wait;             // "" | unconfirmed | tunnel | conflict
};

class ConnectionEngine {
public:
    struct Hooks {
        std::function<double()> now;                                 // monotonic seconds (required)
        std::function<void(double)> sleep;                           // verification pauses; may be a no-op
        std::function<bool(const Credentials&)> persist_credentials; // write to the OS vault; false keeps the old value
        std::function<double()> wall_now;  // persisted cooldown uses wall time; runtime still uses monotonic time
        std::function<bool(const std::string&)> persist_recovery; // atomic write, called under engine lock; must not call engine
        std::function<void()> changed;                               // state changed; called without the engine lock
        std::function<void(const std::string&)> event;               // plain-language, credential-free event text
        std::function<void(const ConnectivityReport&)> report;       // every accepted probe (health samples)
        std::function<bool(const std::string&)> persist_memory;      // atomic write of network-memory.json; false keeps the old file
        std::function<std::string()> random_hex;                     // 32 random hex chars (salt / record id); absent: memory disabled
        std::function<void(double, const CancelToken&)> sleep_cancellable; // optional replacement for sleep, follows operation cancellation
        std::function<bool()> erase_credentials;                    // deletes only this application's saved account
        std::function<bool(const std::string&, const std::vector<std::string>&)> prepare_memory_merge; // metadata before memory commit; no engine calls
    };

    ConnectionEngine(ConnectionService& service, Hooks hooks, bool read_only_session = false);
    ConnectionEngine(const ConnectionEngine&) = delete;
    ConnectionEngine& operator=(const ConnectionEngine&) = delete;

    // ---- configuration (any thread) ----
    void configure(MonitoringMode mode, CampusArea area, ProbeMode probe, std::optional<Credentials> saved);
    bool restore_recovery(const std::optional<std::string>& journal);
    void set_mode(MonitoringMode mode);
    void set_area(CampusArea area);
    void set_probe_mode(ProbeMode mode);
    // A website/strategy change breaks consecutive observations without renewing a spent episode.
    void reset_partial_confirmation();
    void set_draft(const Credentials& draft);
    void revert_draft();
    enum class SaveResult { saved, unchanged, invalid, failed };
    SaveResult save_draft();
    bool delete_account();  // cancels old work; a vault failure pauses automatic login and remains retryable
    void pause_auto_login();
    void resume_auto_login();
    bool trust_current_network();
    void revoke_trust();
    // ---- network memory ----
    bool restore_memory(const std::optional<std::string>& text);  // false: unreadable / unknown schema (keep the file)
    enum class RememberResult { remembered, needsProof, unreliable, readOnly, unreadable, nameConflict, failed, busy, cancelled };
    // Reserve on the UI thread before enqueueing so cancel/path/forget also invalidate queued work.
    class RememberRequest {
    public:
        const std::vector<std::string>& merging_record_ids() const { return merging_record_ids_; }
    private:
        friend class ConnectionEngine;
        ConnectionPolicy::Operation operation_;
        CancelToken token_;
        std::uint64_t epoch_ = 0, memory_generation_ = 0;
        CampusArea area_ = CampusArea::automatic;
        ConnectivityReport selection_report_;
        bool prefer_teaching_ = false;
        bool prefer_dormitory_ = false;
        std::optional<physical::Route> route_;
        std::vector<std::string> merging_record_ids_;
    };
    std::optional<RememberRequest> prepare_remember();
    RememberResult trust_and_remember(const RememberRequest& request);  // worker: fresh discovery, durable save, then trust
    RememberResult trust_and_remember();  // blocking convenience for non-UI callers
    enum class CampusVerificationResult { confirmed, needsProof, busy, cancelled, unavailable };
    class CampusVerificationRequest {
    private:
        friend class ConnectionEngine;
        ConnectionPolicy::Operation operation_;
        CancelToken token_;
        std::uint64_t epoch_ = 0, memory_generation_ = 0;
        CampusArea area_ = CampusArea::automatic;
        ConnectivityReport selection_report_;
        bool prefer_teaching_ = false;
        bool prefer_dormitory_ = false;
        std::optional<physical::Route> route_;
        EngineStatus previous_status_ = EngineStatus::idle;
    };
    std::optional<CampusVerificationRequest> prepare_verify_campus();
    CampusVerificationResult verify_campus(const CampusVerificationRequest& request);
    void forget_network(const std::string& id);  // removes one network and proven duplicate records
    void forget_current_network();        // revokes trust, cancels work, deletes matching records
    void forget_all_networks();
    std::vector<MemoryRecord> remembered_networks() const;
    // Read-only presentation binding; never grants trust or changes matching.
    std::vector<std::string> current_remembered_ids() const;
    void receive_path(const PathSnapshot& path);
    void set_sleeping(bool sleeping);

    // ---- operations (worker thread; blocking). false = not admitted ----
    bool connect();                 // probe; log in only when needed and allowed
    bool check();                   // probe only; never authenticates
    bool connect_even_if_online();  // explicit forced login after an online probe
    bool automatic_round();
    void cancel();
    bool cancel_automatic_probe(); // atomic manual priority; never preempts discovery/submission/verification

    // ---- queries (any thread) ----
    EngineSnapshot snapshot() const;
    bool paused() const;
    AutoLoginState auto_login_state() const;
    int next_check_delay() const;
    Credentials draft() const;

private:
    static constexpr double kCampusServiceRefreshSeconds = 15 * 60;
    static constexpr double kSessionAuditSeconds = 15;
    struct CampusServiceContext {
        std::uint64_t epoch = 0, memory_generation = 0;
        CampusArea area = CampusArea::automatic;
        std::optional<physical::Route> route;
        bool operator==(const CampusServiceContext&) const = default;
    };
    struct CampusServiceRetry {
        CampusServiceContext context;
        double after = 0;
    };
    struct CampusServiceConfirmation {
        CampusServiceContext context;
        double refresh_after = 0;
    };
    bool run(OperationKind kind, bool may_authenticate, bool force);
    void perform(const ConnectionPolicy::Operation& op, const CancelToken& token, bool may_authenticate, bool force);
    void confirm_remembered_campus_service(const ConnectionPolicy::Operation& op, const CancelToken& token,
                                          const ConnectivityReport& report, std::uint64_t memory_generation);
    void flush();
    void emit(std::string text);
    double now() const { return hooks_.now(); }

    // helpers; caller holds mutex_
    bool persist_recovery_locked();
    bool paused_locked() const;
    std::optional<HoldReason> pause_reason_locked() const;
    AutoLoginState auto_login_locked() const;
    std::optional<std::uint64_t> generation_for(const Credentials& credentials) const;
    void preserve_pending_submission_locked();
    void cancel_locked(bool mark_cancelled);
    void set_online_locked(bool settle_uncertain = true);
    void reset_partial_episode_locked();
    int next_check_delay_locked() const;
    void accept_report_locked(const ConnectivityReport& report);
    void new_epoch_locked();
    bool can_authenticate_locked(OperationKind kind) const;
    void handle_attempt_locked(OperationKind kind, CampusArea target, std::optional<std::uint64_t> gen,
                               const LoginAttempt& attempt);
    void refresh_memory_locked();
    bool memory_ambiguous_locked() const { return tunnel_ || multiple_physical_ || descriptors_.size() > 1; }
    std::optional<CampusArea> remembered_entry_locked() const;
    std::optional<CampusRoutingResolution> current_routing_locked() const;
    bool needs_dormitory_eligibility_locked() const;
    bool needs_memory_update_locked() const;
    Discovery required_campus_verification_locked() const;
    void start_discovery_locked();
    void record_routing_locked(CampusArea choice, const ConnectivityReport& report,
                               const CampusDiscovery& discovery, const CampusRoutingResolution& routing);
    std::optional<CampusArea> verified_scope_locked(CampusArea choice, const ConnectivityReport& report,
                                                   const CampusDiscovery& discovery, bool bound, bool prefer_teaching, bool prefer_dormitory = false) const;
    std::optional<AuthenticationScope> authentication_scope_locked() const;
    std::optional<double> cooldown_remaining_locked() const;
    bool can_prioritize_recovery_locked() const;
    std::string remember_blocker_locked() const;
    RememberResult remember_rejection_locked() const;
    bool owns_remember_locked(const RememberRequest& request) const;
    void touch_memory_locked(CampusArea area, bool authenticated = false);
    bool owns_campus_verification_locked(const CampusVerificationRequest& request) const;
    bool can_verify_campus_locked() const;
    CampusServiceContext campus_service_context_locked() const;
    bool automatic_campus_verification_eligible_locked() const;
    void invalidate_campus_service_locked();
    bool commit_memory_locked(NetworkMemory next, const std::vector<std::string>& ids_to_suppress,
                              const std::string& freshly_remembered = {});

    ConnectionService& service_;
    Hooks hooks_;
    const bool read_only_;
    mutable std::mutex mutex_;
    ConnectionPolicy policy_;
    CancelToken token_;
    EngineStatus status_ = EngineStatus::idle;
    std::optional<ConnectivityReport> last_report_;
    bool online_ = false;
    int consecutive_partial_ = 0;
    int consecutive_transport_passes_ = 0;
    bool partial_attempt_used_ = false;  // one recovery round per sustained partial episode
    MonitoringMode mode_ = MonitoringMode::manual;
    CampusArea area_ = CampusArea::automatic;
    ProbeMode probe_mode_ = ProbeMode::quick;
    std::optional<Credentials> saved_;
    Credentials draft_;
    std::uint64_t generation_ = 0;
    std::uint64_t epoch_ = 1;
    std::optional<std::string> identity_;
    bool link_active_ = false;
    bool constrained_ = false;
    bool tunnel_ = false;
    bool multiple_physical_ = false;
    bool sleeping_ = false;
    std::string interfaces_;
    std::string system_interfaces_;
    bool requires_physical_binding_ = false;
    std::optional<physical::Route> authentication_route_;
    std::optional<double> rapid_recovery_until_;
    std::optional<double> session_audit_after_;
    bool full_check_pending_ = false;
    bool offer_forced_login_ = false;
    std::optional<LoginAttempt> last_attempt_;
    std::optional<CampusArea> last_success_area_;
    std::optional<CampusDiscovery> last_discovery_;
    std::vector<std::string> pending_events_;
    bool pending_change_ = false;
    bool recovery_initialized_ = false;
    bool recovery_write_blocked_ = false;
    bool recovery_submission_pending_ = false;
    std::optional<double> recovery_deadline_wall_;
    std::string last_recovery_json_;
    NetworkMemory memory_;
    bool memory_ok_ = false;
    bool memory_unreadable_ = false;
    bool memory_write_failed_ = false;
    std::vector<NetworkDescriptor> descriptors_;
    MatchLevel memory_level_ = MatchLevel::none;
    MatchLevel dormitory_memory_level_ = MatchLevel::none;
    std::string memory_record_, memory_area_;
    std::string dormitory_memory_record_;
    std::set<std::string> suppressed_;
    std::uint64_t memory_generation_ = 0;
    std::optional<std::uint64_t> remembering_operation_;
    std::optional<std::uint64_t> campus_verification_operation_;
    std::optional<std::uint64_t> automatic_campus_verification_operation_;
    std::optional<CampusServiceConfirmation> confirmed_campus_service_;
    std::optional<CampusServiceRetry> campus_service_retry_;
    bool campus_verification_failed_ = false;
    bool conflicting_campus_hints_ = false;
    std::optional<std::uint64_t> credential_operation_;
    std::uint64_t discovery_revision_ = 0;
    std::optional<CampusRoutingResolution> last_routing_;
    std::optional<CampusDiscovery> routing_discovery_;
    CampusArea routing_area_ = CampusArea::automatic;
    std::uint64_t routing_epoch_ = 0;
    std::optional<CampusPortalHint> routing_apple_hint_, routing_microsoft_hint_;
};

std::string mask_account(const std::string& account);

}  // namespace szu
