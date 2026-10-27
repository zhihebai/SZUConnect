// Owns the connection engine and every platform service; all public methods run on the UI thread.
// Engine work happens on one worker thread; hooks post kMsgChanged back to the window.
#pragma once
#include "szu/win/text.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "szu/engine.hpp"
#include "szu/environment.hpp"
#include "szu/internet.hpp"
#include "szu/presentation.hpp"
#include "szu/win/network.hpp"
#include "szu/win/service.hpp"
#include "szu/win/system.hpp"
#include "theme.hpp"
#include "runtime.hpp"

namespace szu::app {

inline constexpr UINT kMsgChanged = WM_APP + 1;
inline constexpr UINT kMsgNetwork = WM_APP + 2;
inline constexpr UINT kMsgTray = WM_APP + 3;
inline constexpr UINT kMsgActivateWidget = WM_APP + 4;
inline constexpr UINT_PTR kTimerAuto = 1, kTimerNetwork = 2, kTimerPersist = 3, kTimerSpin = 4, kTimerFeedback = 5, kTimerWake = 6, kTimerWiFi = 7, kTimerUiClock = 8;

inline constexpr const char* kVersion = "1.0.0";
inline constexpr const char* kBuild = "34";

struct Settings {
    MonitoringMode mode = MonitoringMode::manual;  // a fresh install never checks or logs in on its own
    CampusArea area = CampusArea::automatic;
    ProbeMode probe = ProbeMode::quick;
    Appearance appearance = Appearance::system;
    std::string tutorial = "none";  // none | skipped | completed
};

struct CompatState {
    bool running = false;
    bool done = false;
    bool current_path_included = false;
    compat::Input input;
    compat::Verdict verdict;
};

struct ProxyAssistantSuggestionState {
    unsigned connection_failures = 0;
    unsigned campus_failures = 0;
    bool suggested = false;
    bool physical_unidentified = false;
};


class AppController {
public:
    using AutoTimerArm = std::function<bool(UINT)>;
    using ForceLoginConfirmation = std::function<bool(HWND, const std::wstring&, const std::wstring&)>;
    using DiagnosticsSavePicker = std::function<std::optional<std::wstring>(HWND)>;
    AppController(HWND window, RuntimeMode mode, std::unique_ptr<RuntimeServices> runtime = nullptr,
                  AutoTimerArm auto_timer_arm = {}, ForceLoginConfirmation force_login_confirmation = {},
                  DiagnosticsSavePicker diagnostics_save_picker = {});
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;
    void start();
    void shutdown();

    EngineSnapshot snapshot() const {
        auto state = engine_->snapshot();
        if (network_refresh_pending_) { state.can_remember = false; state.can_verify_campus = false; }
        if (manual_connection_pending_ || (!state.busy && manual_admission_queued_)) {
            state.busy = true; state.status = EngineStatus::checking; state.busy_kind = OperationKind::manual;
        }
        return state;
    }
    const Settings& settings() const { return settings_; }
    bool settings_writable() const { return settings_writable_; }
    const records::EventLog& events() const { return events_; }
    bool is_failure_event(const std::string& text) const;
    const records::HealthLog& health() const { return health_; }
    const win::NetworkSnapshot& network() const { return network_; }
    std::string physical_egress_status() const;
    const proxy::Summary& proxy_summary() const { return proxy_; }
    CompatState compat() const;
    std::optional<InternetCheckReport> internet_report() const { return internet_service_.latest(); }
    std::optional<InternetCheckActivity> internet_activity() const { return internet_service_.activity(); }
    std::optional<ConnectivityReport> system_connectivity() const;
    std::optional<double> system_checked_at() const;
    std::optional<ProbeMode> system_checked_mode() const;
    bool system_connectivity_running() const;  // independent display probe, never authentication-engine admission
    const std::vector<WebsiteTarget>& website_targets() const { return website_preferences_.current().targets; }
    bool website_follow_system_proxy() const { return website_preferences_.current().follow_system_proxy; }
    const std::string& website_configuration_error() const { return website_configuration_error_; }
    bool replace_website_targets(const std::vector<WebsiteTarget>& targets, bool select_complete = true);
    void set_website_proxy(bool follow_system_proxy);
    bool read_only() const { return read_only_; }
    bool offline_smoke() const { return runtime_->mode() == RuntimeMode::offlineSmoke; }
    RuntimeEvidence& runtime_evidence() const { return runtime_->evidence(); }
    bool fake_auth_passed() const { return runtime_->fake_auth_passed(); }
    bool launch_enabled() const { return launch_enabled_; }
    bool launch_approved() const { return launch_approved_; }
    const std::string& notice() const { return notice_; }  // copy key of the latest storage / vault problem
    void clear_notice() { notice_.clear(); }
    Credentials draft() const { return engine_->draft(); }
    std::optional<double> last_checked() const { return last_checked_; }
    std::optional<double> next_check() const { return auto_armed_ ? next_due_ : std::nullopt; }
    static double wall_now();

    void check();
    void connect();
    void force_login();
    void cancel();
    void set_mode(MonitoringMode mode);
    void set_area(CampusArea area);
    void set_probe(ProbeMode mode);
    void pause();
    void resume();
    bool trust();
    void revoke_trust();
    // Explicit, revocable approval to remember the current campus network.
    bool remember_network();  // reserves a fresh verification and enqueues credential-free work
    bool verify_campus_service();  // checks an existing memory without changing its scope or recovery schedule
    std::string memory_verification_message() const;  // local, credential-free copy key beside the memory action
    void forget_network();
    void forget_network(const std::string& id);
    void forget_all_networks();
    std::vector<MemoryRecord> remembered_networks() const { return engine_->remembered_networks(); }
    std::string network_name(const std::string& id) const { return network_labels_.display_name(id); }
    std::string network_display_name(const std::string& id) const;
    std::optional<std::string> current_network_record_id() const;
    std::string network_kind(const std::string& id) const;
    std::string authentication_target_name() const;
    std::string physical_system_primary_name() const;
    std::vector<std::string> physical_connection_names() const;
    bool physical_identification_needed() const;
    std::string physical_identification_detail() const;
    std::string identification_feedback() const { return identification_feedback_; }
    void refresh_physical_identification();
    bool rename_network(const std::string& id, const std::string& name);
    wifi::Preferences wifi_preferences() const;
    std::vector<wifi::Network> wifi_candidates() const;
    std::string wifi_status() const;
    bool wifi_selection_required() const;
    bool wifi_unreadable() const;
    bool wifi_selected(const std::string& id) const;
    std::string wifi_name(const std::string& id) const;
    bool set_wifi_auto_join(bool);
    bool set_wifi_power_on(bool);
    bool set_wifi_selected(const std::string& id, bool);
    bool set_wifi_name(const std::string& id, const std::string& ssid);
    bool is_wifi_record(const std::string& id) const;
    bool can_edit_wifi_name(const std::string& id) const;
    bool network_name_known(const std::string& id) const { return network_labels_.entries().count(id) != 0; }
    void set_draft(const std::string& account, const std::string& password);
    ConnectionEngine::SaveResult save_account();
    bool delete_account();
    bool can_delete_account() const { return !read_only_ && (credential_read_failed_ || engine_->snapshot().has_saved); }
    void login_once();
    void discard_draft();
    void set_appearance(Appearance appearance);
    bool set_launch(bool enabled);
    void refresh_environment();  // reread local connection information without starting a check or login
    std::optional<double> environment_refreshed() const { return environment_refreshed_; }
    bool environment_refresh_incomplete() const { return environment_refresh_incomplete_; }
    void run_compat(bool include_current_path = false);
    void cancel_compat();
    void set_tutorial(const std::string& status);
    bool export_diagnostics(HWND owner);
    void cancel_diagnostics_export();
    bool diagnostics_export_running() const { return diagnostics_export_running_; }
    const std::string& diagnostics_export_feedback() const { return diagnostics_export_feedback_; }
    bool copy_diagnostics(HWND owner);
    std::string diagnostic_summary_text() const;
    std::string diagnostic_export_text() const;
    bool copy_rules(HWND owner);
    void open_proxy_assistant();
    void close_proxy_assistant();
    void set_proxy_config_format(proxyassist::Format);
    void prepare_proxy_config(HWND owner);
    void apply_proxy_config();
    void restore_proxy_config();
    void discard_proxy_config_backup();
    bool copy_proxy_script(HWND owner);
    proxyassist::Format proxy_config_format() const { return proxy_config_format_; }
    const proxyassist::Preview& proxy_config_preview() const { return proxy_config_preview_; }
    const std::string& proxy_config_feedback() const { return proxy_config_feedback_; }
    bool proxy_config_has_backup() const { return proxy_config_has_backup_; }
    ProxyAssistantSuggestionState proxy_assistant_suggestion() const;
    bool should_suggest_proxy_assistant() const { return proxy_assistant_suggestion().suggested; }

    void on_changed();
    void on_network_signal();
    void on_timer(UINT_PTR id);
    void on_suspend();
    void on_resume();

private:
    void post_changed();
    void drain_diagnostics_export();
    bool run_engine(std::function<void()> job, bool automatic = false);
    void seal_fresh_report_environment(const win::NetworkSnapshot& network, double captured_at);
    struct FailureCapture {
        records::FailureContext context;
        std::uint64_t epoch = 0, discovery_revision = 0, report_revision = 0;
        std::uint64_t cancellation_generation = 0;
        std::optional<std::uint64_t> credential_generation = std::nullopt;
        std::size_t memory_count = 0;
        bool remembered = false, user_paused = false;
        EngineStatus status = EngineStatus::idle;
    };
    FailureCapture capture_failure_context(bool automatic) const;
    void record_operation_failure(const FailureCapture& capture, const char* result = nullptr);
    void queue_manual_connection(bool force, std::uint64_t confirmed_epoch = 0, std::uint64_t confirmed_report = 0);
    void invalidate_manual_connection(bool diagnostic_boundary = true);
    void schedule_auto(int seconds);
    void arm_auto_timer(double deadline);
    void retry_auto_timer();
    void refresh_network();
    void apply_network_snapshot(win::NetworkSnapshot latest, std::optional<double> captured_at = std::nullopt);
    void refresh_proxy();
    void refresh_network_labels();
    bool save_network_labels(const NetworkLabels& labels);
    bool reload_network_labels();  // UI thread; refresh before deriving any sidecar write
    void update_wifi_context();
    bool save_wifi_preferences(wifi::Preferences, bool revoking, bool merge_preflight = false);
    void run_wifi_recovery();
    void load();
    void save_settings();
    bool save_website_preferences(const WebsitePreferences& preferences);
    void persist_logs();
    records::ExportContext export_context() const;
    void clear_system_connectivity(bool recheck_after_network_refresh = false);
    void refresh_system_connectivity(bool recheck_only = false);
    void check_with_system_connectivity();
    void set_memory_verification_message(std::string key);
    void clear_memory_verification_message();
    bool memory_verification_pending() const;
    void invalidate_memory_verification(const char* key);
    void sync_memory_verification_epoch();
    void restore_campus_verification_schedule();
    void sync_proxy_assistant_context(const EngineSnapshot& state);
    void invalidate_proxy_assistant_round(bool reset_failures = false);
    void record_proxy_assistant_physical_report(const ConnectivityReport& report);
    void record_proxy_assistant_system_report(std::uint64_t generation, const std::optional<ConnectivityReport>& report);
    void finish_proxy_assistant_round();
    void evaluate_proxy_assistant_round_locked();
    void record_proxy_assistant_campus_check(std::uint64_t revision, std::uint64_t campus_revision, const compat::Input& input);

    HWND window_;
    bool read_only_;
    std::unique_ptr<RuntimeServices> runtime_;
    InternetCheckService internet_service_;
    WebsitePreferencesState website_preferences_;
    std::string website_configuration_error_;
    std::unique_ptr<ConnectionEngine> engine_;
    std::atomic<bool> shutting_down_{false};
    Settings settings_;
    bool settings_writable_ = true, events_writable_ = true, health_writable_ = true;
    records::EventLog events_;
    records::HealthLog health_;
    win::NetworkSnapshot network_;
    NetworkLabels network_labels_;
    bool network_labels_writable_ = true;
    mutable std::mutex wifi_mutex_;
    wifi::Preferences wifi_preferences_;
    std::vector<wifi::Network> wifi_candidates_;
    wifi::Schedule wifi_schedule_;
    bool wifi_unreadable_ = false;
    proxy::Summary proxy_;
    bool launch_enabled_ = false;
    bool launch_approved_ = true;
    bool credential_read_failed_ = false;
    std::string notice_;
    proxyassist::Format proxy_config_format_ = proxyassist::Format::clashVergeScript;
    proxyassist::Preview proxy_config_preview_;
    std::string proxy_config_feedback_;
    bool proxy_config_has_backup_ = false;
    struct ProxyAssistantContext {
        std::uint64_t epoch;
        std::string physical_identity;
        bool operator==(const ProxyAssistantContext&) const = default;
    };
    std::optional<ProxyAssistantContext> proxy_assistant_context(const EngineSnapshot& state) const;
    struct ProxyAssistantRound {
        std::uint64_t system_generation;
        std::uint64_t revision;
        bool worker_finished = false;
        bool system_finished = false;
        std::optional<ConnectivityReport> physical, system;
    };
    mutable std::mutex proxy_suggestion_mutex_;
    std::optional<ProxyAssistantContext> proxy_suggestion_context_;
    ProxyAssistantSuggestionState proxy_suggestion_state_;
    std::uint64_t proxy_suggestion_revision_ = 0;
    std::uint64_t proxy_campus_discovery_revision_ = 0;
    std::uint64_t proxy_campus_check_revision_ = 0;
    std::optional<ProxyAssistantRound> proxy_suggestion_round_;
    std::atomic<bool> proxy_physical_link_active_{false};
    std::optional<double> last_checked_;
    std::optional<double> environment_refreshed_;
    bool environment_refresh_incomplete_ = false;
    std::string identification_feedback_;
    std::atomic<std::uint64_t> identification_generation_{0};
    std::optional<double> next_due_;
    std::optional<double> auto_timer_retry_deadline_;
    AutoTimerArm auto_timer_arm_;
    ForceLoginConfirmation force_login_confirmation_;
    DiagnosticsSavePicker diagnostics_save_picker_;
    CancelToken diagnostics_export_token_;
    std::thread diagnostics_export_worker_;
    std::mutex diagnostics_export_mutex_;
    std::optional<std::string> diagnostics_export_inbox_;
    bool diagnostics_export_running_ = false;
    std::string diagnostics_export_feedback_;
    bool auto_armed_ = false;
    bool network_refresh_pending_ = false;  // UI-thread gate until debounced physical identity is captured
    bool diagnostic_network_fresh_ = false; // display/export freshness only; wake waits for a new applied capture
    bool network_refresh_timer_armed_ = false;  // UI-thread scheduling, independent of the admission gate
    bool logs_dirty_ = false;
    std::vector<std::string> failure_markers_;

    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::atomic<bool> engine_busy_{false};
    std::atomic<bool> manual_queued_{false}; // one pending/active foreground request, including behind Wi-Fi work
    std::atomic<std::uint64_t> manual_connection_generation_{0};
    std::atomic<std::uint64_t> diagnostic_cancel_generation_{0};
    std::atomic<std::uint64_t> connectivity_revision_{0};
    std::atomic<bool> manual_admission_queued_{false};
    bool manual_connection_pending_ = false;  // UI-owned capture-to-admission handoff

    std::mutex inbox_mutex_;
    std::vector<records::Event> inbox_events_;
    struct CompletedReport { records::Sample sample; std::uint64_t epoch; };
    std::vector<CompletedReport> inbox_reports_;
    std::optional<records::SampleContext> active_report_context_; // serial worker only
    std::uint64_t active_report_epoch_ = 0;
    std::optional<FailureCapture> active_failure_context_ = std::nullopt; // serial worker only
    double network_observed_at_ = 0;
    struct RememberFeedback {
        ConnectionEngine::RememberResult result;
        std::uint64_t generation;
        std::uint64_t epoch;
    };
    std::optional<RememberFeedback> inbox_remember_result_;
    struct CampusFeedback {
        ConnectionEngine::CampusVerificationResult result;
        std::uint64_t generation;
        std::uint64_t epoch;
        std::uint64_t discovery_revision;
    };
    std::optional<CampusFeedback> inbox_campus_result_;
    struct ManualConnectionCapture {
        std::uint64_t generation;
        std::uint64_t confirmed_epoch;
        std::uint64_t confirmed_report;
        bool force;
        double captured_at = 0;
        std::optional<win::NetworkSnapshot> network;
    };
    std::optional<ManualConnectionCapture> inbox_manual_connection_;
    bool inbox_labels_refresh_ = false;
    struct CampusVerificationSchedule {
        bool running;
        std::optional<double> deadline;
        MonitoringMode mode;
    };
    std::optional<CampusVerificationSchedule> campus_verification_schedule_;
    bool campus_service_only_verification_ = false;
    std::string memory_verification_message_;
    std::uint64_t memory_verification_generation_ = 0;
    std::optional<std::uint64_t> memory_verification_epoch_;
    std::optional<CampusArea> memory_verification_area_;
    std::uint64_t memory_verification_discovery_revision_ = 0;
    std::vector<std::string> memory_verification_merge_ids_;
    std::atomic<bool> posted_{false};

    mutable std::mutex checks_mutex_;
    CompatState compat_;
    std::uint64_t compat_generation_ = 0;
    CancelToken compat_token_;
    std::thread compat_thread_;
    mutable std::mutex system_probe_mutex_;
    std::optional<ConnectivityReport> system_report_;
    std::optional<double> system_checked_at_;
    bool system_probe_running_ = false;
    bool system_probe_recheck_pending_ = false;  // existing display check invalidated by a network notification
    CancelToken system_probe_token_;
    std::uint64_t system_probe_generation_ = 0;
    struct SystemProbeRequest {
        CancelToken token;
        std::uint64_t generation;
        ProbeMode mode;
    };
    std::optional<SystemProbeRequest> system_probe_pending_;
    std::condition_variable system_probe_cv_;
    std::thread system_probe_worker_;
    bool system_probe_stopping_ = false;

    // Worker-thread continuity only. A changed visible SSID revokes a session; it never grants trust.
    std::optional<std::string> last_auto_wifi_ssid_;
    std::optional<std::string> last_auto_identity_;
};

// Enum <-> persisted string helpers shared with the views.
std::optional<MonitoringMode> parse_mode(std::string_view text);
std::optional<CampusArea> parse_area(std::string_view text);
std::optional<ProbeMode> parse_probe(std::string_view text);

}  // namespace szu::app
