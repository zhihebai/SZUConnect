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

inline constexpr const char* kVersion = "0.10.1";
inline constexpr const char* kBuild = "32";

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
    compat::Input input;
    compat::Verdict verdict;
};


class AppController {
public:
    AppController(HWND window, RuntimeMode mode, std::unique_ptr<RuntimeServices> runtime = nullptr);
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;
    void start();
    void shutdown();

    EngineSnapshot snapshot() const {
        auto state = engine_->snapshot();
        if (network_refresh_pending_) state.can_remember = false;
        return state;
    }
    const Settings& settings() const { return settings_; }
    bool settings_writable() const { return settings_writable_; }
    const records::EventLog& events() const { return events_; }
    const records::HealthLog& health() const { return health_; }
    const win::NetworkSnapshot& network() const { return network_; }
    const proxy::Summary& proxy_summary() const { return proxy_; }
    CompatState compat() const;
    std::optional<InternetCheckReport> internet_report() const { return internet_service_.latest(); }
    std::optional<ConnectivityReport> system_connectivity() const;
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
    std::string memory_verification_message() const;  // local, credential-free copy key beside the memory action
    void forget_network();
    void forget_all_networks();
    std::vector<MemoryRecord> remembered_networks() const { return engine_->remembered_networks(); }
    std::string network_name(const std::string& id) const { return network_labels_.display_name(id); }
    bool rename_network(const std::string& id, const std::string& name);
    wifi::Preferences wifi_preferences() const;
    std::vector<wifi::Network> wifi_candidates() const;
    std::string wifi_status() const;
    bool wifi_unreadable() const;
    bool wifi_selected(const std::string& id) const;
    std::string wifi_name(const std::string& id) const;
    bool set_wifi_auto_join(bool);
    bool set_wifi_power_on(bool);
    bool set_wifi_selected(const std::string& id, bool);
    bool set_wifi_name(const std::string& id, const std::string& ssid);
    bool is_wifi_record(const std::string& id) const;
    bool network_name_known(const std::string& id) const { return network_labels_.entries().count(id) != 0; }
    void set_draft(const std::string& account, const std::string& password);
    ConnectionEngine::SaveResult save_account();
    void login_once();
    void discard_draft();
    void set_appearance(Appearance appearance);
    bool set_launch(bool enabled);
    void refresh_environment();  // reread local connection information without starting a check or login
    void run_compat();
    void cancel_compat();
    void set_tutorial(const std::string& status);
    bool export_diagnostics(HWND owner);
    bool copy_diagnostics(HWND owner);
    bool copy_rules(HWND owner);

    void on_changed();
    void on_network_signal();
    void on_timer(UINT_PTR id);
    void on_suspend();
    void on_resume();

private:
    void post_changed();
    void run_engine(std::function<void()> job, bool automatic = false);
    void schedule_auto(int seconds);
    void refresh_network();
    void refresh_proxy();
    void refresh_network_labels();
    bool save_network_labels(const NetworkLabels& labels);
    void update_wifi_context();
    bool save_wifi_preferences(const wifi::Preferences&, bool revoking);
    void run_wifi_recovery();
    void load();
    void save_settings();
    bool save_website_preferences(const WebsitePreferences& preferences);
    void persist_logs();
    bool is_failure_event(const std::string& text) const;
    records::ExportContext export_context() const;
    void clear_system_connectivity(bool recheck_after_network_refresh = false);
    void refresh_system_connectivity(bool recheck_only = false);
    void check_with_system_connectivity();
    void set_memory_verification_message(std::string key);
    void clear_memory_verification_message();
    bool memory_verification_pending() const;
    void invalidate_memory_verification(const char* key);
    void sync_memory_verification_epoch();

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
    std::string notice_;
    std::optional<double> last_checked_;
    std::optional<double> next_due_;
    bool auto_armed_ = false;
    bool network_refresh_pending_ = false;  // UI-thread gate until debounced physical identity is captured
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

    std::mutex inbox_mutex_;
    std::vector<std::string> inbox_events_;
    std::vector<ConnectivityReport> inbox_reports_;
    struct RememberFeedback {
        ConnectionEngine::RememberResult result;
        std::uint64_t generation;
        std::uint64_t epoch;
    };
    std::optional<RememberFeedback> inbox_remember_result_;
    std::string memory_verification_message_;
    std::uint64_t memory_verification_generation_ = 0;
    std::optional<std::uint64_t> memory_verification_epoch_;
    std::atomic<bool> posted_{false};

    mutable std::mutex checks_mutex_;
    CompatState compat_;
    std::uint64_t compat_generation_ = 0;
    CancelToken compat_token_;
    std::thread compat_thread_;
    mutable std::mutex system_probe_mutex_;
    std::optional<ConnectivityReport> system_report_;
    bool system_probe_running_ = false;
    bool system_probe_recheck_pending_ = false;  // existing display check invalidated by a network notification
    CancelToken system_probe_token_;
    std::uint64_t system_probe_generation_ = 0;
    struct SystemProbeRequest {
        CancelToken token;
        std::uint64_t generation;
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
