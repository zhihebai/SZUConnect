#include "controller.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <utility>

#include "shared_copy.hpp"
#include "szu/json.hpp"

namespace szu::app {

using Microsoft::WRL::ComPtr;

namespace {
bool force_login_available(const EngineSnapshot& state) {
    return !state.busy && state.last_report && state.last_report->online() &&
        state.last_report->complete_result != CompleteCheckResult::cancelled &&
        !state.last_report->has_recovery_evidence() &&
        (state.offer_forced_login || state.last_report->complete_partial());
}
}

std::optional<MonitoringMode> parse_mode(std::string_view text) {
    for (MonitoringMode m : {MonitoringMode::manual, MonitoringMode::observe, MonitoringMode::recover})
        if (to_string(m) == text) return m;
    return std::nullopt;
}
std::optional<CampusArea> parse_area(std::string_view text) {
    for (CampusArea a : {CampusArea::automatic, CampusArea::teaching, CampusArea::dormitory})
        if (to_string(a) == text) return a;
    return std::nullopt;
}
std::optional<ProbeMode> parse_probe(std::string_view text) {
    for (ProbeMode p : {ProbeMode::quick, ProbeMode::complete})
        if (to_string(p) == text) return p;
    return std::nullopt;
}

namespace {
std::string_view appearance_name(Appearance a) {
    switch (a) {
        case Appearance::light: return "light";
        case Appearance::dark: return "dark";
        default: return "system";
    }
}
std::string prefix_before_brace(std::string_view text) {
    const auto brace = text.find('{');
    return std::string(text.substr(0, brace));
}
std::optional<Settings> decode_settings(const std::string& text) {
    const auto root = json::parse(text);
    if (!root || !root->is_object()) return std::nullopt;
    // Early v1 files may omit the version. An explicit unknown version must
    // remain recoverable rather than being silently rewritten as v1.
    if (const auto* version = root->find("formatVersion"); version && (!version->is_number() || version->as_number() != 1))
        return std::nullopt;
    Settings settings;
    if (const auto* value = root->find("monitoringMode")) {
        const auto parsed = value->is_string() ? parse_mode(value->as_string()) : std::nullopt;
        if (!parsed) return std::nullopt;
        settings.mode = *parsed;
    }
    if (const auto* value = root->find("area")) {
        const auto parsed = value->is_string() ? parse_area(value->as_string()) : std::nullopt;
        if (!parsed) return std::nullopt;
        settings.area = *parsed;
    }
    if (const auto* value = root->find("probeMode")) {
        const auto parsed = value->is_string() ? parse_probe(value->as_string()) : std::nullopt;
        if (!parsed) return std::nullopt;
        settings.probe = *parsed;
    }
    if (const auto* value = root->find("appearanceMode")) {
        if (!value->is_string() || (value->as_string() != "light" && value->as_string() != "dark" && value->as_string() != "system"))
            return std::nullopt;
        settings.appearance = value->as_string() == "light" ? Appearance::light : value->as_string() == "dark" ? Appearance::dark : Appearance::system;
    }
    if (const auto* tutorial = root->find("tutorial")) {
        if (!tutorial->is_object()) return std::nullopt;
        if (const auto* value = tutorial->find("status")) {
            if (!value->is_string() || (value->as_string() != "none" && value->as_string() != "skipped" && value->as_string() != "completed"))
                return std::nullopt;
            settings.tutorial = value->as_string();
        }
    }
    return settings;
}
}  // namespace

double AppController::wall_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

AppController::AppController(HWND window, RuntimeMode mode, std::unique_ptr<RuntimeServices> runtime,
                             AutoTimerArm auto_timer_arm, ForceLoginConfirmation force_login_confirmation,
                             DiagnosticsSavePicker diagnostics_save_picker)
    : window_(window), read_only_(mode == RuntimeMode::observeOnly),
    runtime_(runtime ? std::move(runtime) : make_runtime(mode)), internet_service_(*runtime_, [this](const std::string& url, bool follow, const CancelToken& token) {
        return runtime_->check_website(url, follow, token);
    }) {
    diagnostics_save_picker_ = std::move(diagnostics_save_picker);
    auto_timer_arm_ = auto_timer_arm ? std::move(auto_timer_arm) : AutoTimerArm([this](UINT milliseconds) {
        return SetTimer(window_, kTimerAuto, milliseconds, nullptr) != 0;
    });
    force_login_confirmation_ = force_login_confirmation ? std::move(force_login_confirmation) :
        ForceLoginConfirmation([](HWND owner, const std::wstring& title, const std::wstring& body) {
            return MessageBoxW(owner, body.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
        });
    ConnectionEngine::Hooks hooks;
    hooks.now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    hooks.sleep_cancellable = [](double seconds, const CancelToken& token) { token.wait_for(seconds); };
    hooks.persist_credentials = [this](const Credentials& c) { return !read_only_ && runtime_->write_credentials(c); };
    hooks.erase_credentials = [this] { return !read_only_ && runtime_->erase_credentials(); };
    hooks.wall_now = [] { return wall_now(); };
    hooks.persist_recovery = [this](const std::string& data) { return runtime_->write(L"recovery.json", data); };
    hooks.persist_memory = [this](const std::string& data) { return runtime_->write(L"network-memory.json", data); };  // atomic; failure keeps the old file
    hooks.prepare_memory_merge = [this](const std::string& survivor, const std::vector<std::string>& sources) {
        if (read_only_) return false;
        std::string text;
        const auto status = runtime_->read(L"network-labels.json", text);
        if (status == win::Storage::ReadStatus::failed) return false;
        const auto decoded = status == win::Storage::ReadStatus::missing ? std::optional<NetworkLabels>(NetworkLabels{}) : NetworkLabels::decode(text);
        if (!decoded) return false;
        auto labels = *decoded;
        const auto result = labels.preserve_merged_metadata(survivor, sources);
        if (result == NetworkLabels::MergeResult::conflict || result == NetworkLabels::MergeResult::unavailable) return false;
        if (result == NetworkLabels::MergeResult::updated && !runtime_->write(L"network-labels.json", labels.encode())) return false;
        if (result == NetworkLabels::MergeResult::updated) {
            std::lock_guard<std::mutex> lock(inbox_mutex_); inbox_labels_refresh_ = true;
        }
        return true;
    };
    hooks.random_hex = [this] { return runtime_->random_hex(16); };
    hooks.changed = [this] { post_changed(); };
    hooks.event = [this](const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_events_.push_back(records::Event{wall_now(), text, false});
        }
        post_changed();
    };
    hooks.report = [this](const ConnectivityReport& report) {
        ++connectivity_revision_;
        record_proxy_assistant_physical_report(report);
        int site_total = -1, site_passed = -1;
        const auto internet = internet_service_.latest();
        if (internet && internet->selected_count <= 12 && report.mode == ProbeMode::complete && internet->base.mode == report.mode &&
            internet->base.apple.state == report.apple.state && internet->base.microsoft.state == report.microsoft.state &&
            internet->base.apple.elapsed == report.apple.elapsed && internet->base.microsoft.elapsed == report.microsoft.elapsed) {
            site_total = static_cast<int>(internet->selected_count);
            const int base_passed = (report.apple.succeeded() ? 1 : 0) + (report.microsoft.succeeded() ? 1 : 0);
            site_passed = std::clamp(static_cast<int>(internet->passed_count()) - base_passed, 0, site_total);
        }
        CompletedReport completed{records::sample_from(report, wall_now(), site_total, site_passed, active_report_context_), active_report_epoch_};
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_reports_.push_back(std::move(completed));
        }
        post_changed();
    };
    if (read_only_) { hooks.persist_credentials = {}; hooks.erase_credentials = {}; hooks.persist_recovery = {}; hooks.persist_memory = {}; }
    engine_ = std::make_unique<ConnectionEngine>(internet_service_, std::move(hooks), read_only_);
    // Failure detection for "只看失败": fixed parts of the failure event templates.
    for (const char* key : {"event.failed", "event.manualCheckFailed"}) {
        const std::string prefix = prefix_before_brace(copy::text(key));
        if (!prefix.empty()) failure_markers_.push_back(prefix);
    }
    failure_markers_.emplace_back(copy::text("event.offlineAfterOnline"));
    const std::string hold = std::string(copy::text("event.holdCreated"));
    const auto close = hold.find('}');
    const auto colon = hold.find("：");
    if (close != std::string::npos && colon != std::string::npos && colon > close + 1)
        failure_markers_.push_back(hold.substr(close + 1, colon - close - 1));
    // Preserve known failure phrases across wording changes.
    for (const char* legacy : {"登录未完成：", "登录没有完成：", "手动检测未通过：", "外网检测暂未通过，等待复测确认。",
                             "外网检测这次没通过，稍后会再测。", "登录被拒绝，这个区域的自动重连先暂停了", "拒绝了登录"})
        failure_markers_.emplace_back(legacy);
}

AppController::~AppController() { shutdown(); }

void AppController::start() {
    load();
    Credentials saved;
    std::optional<Credentials> stored;
    switch (read_only_ ? win::CredentialVault::ReadStatus::missing : runtime_->read_credentials(saved)) {
        case win::CredentialVault::ReadStatus::found: stored = saved; break;
        case win::CredentialVault::ReadStatus::failed: credential_read_failed_ = true; notice_ = "account.readFailed"; break;
        case win::CredentialVault::ReadStatus::missing: break;
    }
    win::wipe(saved.password);
    engine_->configure(settings_.mode, settings_.area, settings_.probe, stored);
    if (stored) win::wipe(stored->password);
    std::string journal;
    const auto journal_status = runtime_->read(L"recovery.json", journal);
    const bool restored = engine_->restore_recovery(journal_status == win::Storage::ReadStatus::missing
        ? std::nullopt : std::optional<std::string>(journal));
    if (!restored) { if (!read_only_) runtime_->quarantine(L"recovery.json"); notice_ = "error.storage"; }
    {
        // network-memory.json is never quarantined or rewritten when unreadable: the user's approvals stay recoverable.
        std::string memory_text;
        switch (runtime_->read(L"network-memory.json", memory_text)) {
            case win::Storage::ReadStatus::missing: engine_->restore_memory(std::nullopt); break;
            case win::Storage::ReadStatus::ok: if (!engine_->restore_memory(memory_text)) notice_ = "memory.unreadable"; break;
            case win::Storage::ReadStatus::failed: engine_->restore_memory(std::string()); notice_ = "memory.unreadable"; break;
        }
    }
    {
        std::string labels_text;
        const auto status = runtime_->read(L"network-labels.json", labels_text);
        if (status == win::Storage::ReadStatus::ok) {
            if (const auto decoded = NetworkLabels::decode(labels_text)) network_labels_ = *decoded;
            else network_labels_writable_ = false;
        } else if (status == win::Storage::ReadStatus::failed) network_labels_writable_ = false;
    }
    {
        std::string text;
        const auto status = runtime_->read(L"wifi-recovery.json", text);
        if (status == win::Storage::ReadStatus::ok) {
            if (const auto preferences = wifi::Preferences::decode(text)) wifi_preferences_ = *preferences;
            else wifi_unreadable_ = true;
        } else if (status == win::Storage::ReadStatus::failed) wifi_unreadable_ = true;
    }
    launch_enabled_ = runtime_->launch_enabled();
    launch_approved_ = runtime_->launch_approved();
    refresh_proxy();
    refresh_network();
    if (network_.current_label && network_.current_label->kind == "wifi")
        last_auto_wifi_ssid_ = network_.current_label->ssid;
    last_auto_identity_ = network_.path.identity;
    // System-path evidence is presentation only. Its independently managed worker must
    // never hold the serial authentication worker while a public endpoint is slow.
    system_probe_worker_ = std::thread([this] {
        while (true) {
            SystemProbeRequest request{};
            {
                std::unique_lock<std::mutex> lock(system_probe_mutex_);
                system_probe_cv_.wait(lock, [this] { return system_probe_stopping_ || system_probe_pending_.has_value(); });
                if (system_probe_stopping_) return;
                request = *system_probe_pending_;
                system_probe_pending_.reset();
            }
            const auto report = runtime_->system_connectivity(request.mode, request.token);
            {
                std::lock_guard<std::mutex> lock(system_probe_mutex_);
                if (!system_probe_stopping_ && request.generation == system_probe_generation_ && !request.token.cancelled()) {
                    system_report_ = report;
                    system_checked_at_ = report ? std::optional<double>(wall_now()) : std::nullopt;
                    system_probe_running_ = false;
                    record_proxy_assistant_system_report(request.generation, report);
                }
            }
            post_changed();
        }
    });
    worker_ = std::thread([this] {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(worker_mutex_);
                worker_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                engine_busy_ = true;
            }
            job();
            finish_proxy_assistant_round();
            engine_busy_ = false;
            post_changed();
        }
    });
    runtime_->start_watchers([this] { if (!shutting_down_) PostMessageW(window_, kMsgNetwork, 0, 0); });
    if (!read_only_ && settings_writable_) SetTimer(window_, kTimerWiFi, 2000, nullptr);
    if (settings_.mode != MonitoringMode::manual) schedule_auto(engine_->next_check_delay());
}

void AppController::shutdown() {
    if (!engine_ || shutting_down_.exchange(true)) return;
    diagnostics_export_token_.cancel();
    invalidate_manual_connection();
    ++identification_generation_;
    for (const auto id : {kTimerAuto, kTimerNetwork, kTimerPersist, kTimerWake, kTimerWiFi}) KillTimer(window_, id);
    auto_armed_ = false; next_due_.reset(); auto_timer_retry_deadline_.reset();
    network_refresh_timer_armed_ = false;
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.cancel(); }
    runtime_->stop_watchers();
    engine_->cancel();
    clear_system_connectivity();
    cancel_compat();
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(system_probe_mutex_);
        system_probe_stopping_ = true;
        system_probe_pending_.reset();
    }
    worker_cv_.notify_all();
    system_probe_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (system_probe_worker_.joinable()) system_probe_worker_.join();
    if (compat_thread_.joinable()) compat_thread_.join();
    if (diagnostics_export_worker_.joinable()) diagnostics_export_worker_.join();
    if (logs_dirty_) persist_logs();
}

void AppController::post_changed() {
    if (!shutting_down_ && !posted_.exchange(true)) PostMessageW(window_, kMsgChanged, 0, 0);
}

bool AppController::run_engine(std::function<void()> job, bool automatic) {
    // Admission must happen before the serial worker queue. Waiting until the manual job
    // reaches core::run would leave it blocked behind a hanging background website probe.
    // The engine checks operation kind + stage atomically, so a concurrent transition into
    // discovery/submission/verification cannot be cancelled by this manual request.
    if (!automatic) {
        if (engine_->cancel_automatic_probe()) internet_service_.invalidate();
        else if (engine_->snapshot().busy) return false; // ignore clicks during protected or existing foreground work
    }
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (stopping_) return false;
    if (automatic && (engine_busy_ || !jobs_.empty() || manual_connection_pending_)) return false;  // never queue background rounds behind other work
    if (!automatic && (manual_queued_ || !jobs_.empty() || manual_connection_pending_)) return false;
    records::SampleContext context;
    context.operation = automatic ? "automatic" : "manual";
    context.environment.refreshing = network_refresh_pending_ || !diagnostic_network_fresh_;
    if (!context.environment.refreshing) {
        context.environment.connection = network_.access;
        context.environment.system_proxy = proxy_.state;
        context.environment.tun = network_.tunnel_known ? (network_.path.tunnel ? "detected" : "notDetected") : "unknown";
    }
    auto failure_capture = capture_failure_context(automatic);
    auto stamped_job = [this, context = std::move(context), failure_capture = std::move(failure_capture), job = std::move(job)]() mutable {
        const auto state = engine_->snapshot();
        context.started_at = wall_now();
        context.monitoring_mode = std::string(to_string(state.mode));
        context.area = std::string(to_string(state.area));
        active_report_context_ = std::move(context);
        active_report_epoch_ = state.epoch;
        failure_capture.context.started_at = context.started_at;
        failure_capture.context.monitoring_mode = std::string(to_string(state.mode));
        failure_capture.context.area = std::string(to_string(state.area));
        failure_capture.epoch = state.epoch; failure_capture.discovery_revision = state.discovery_revision;
        failure_capture.report_revision = connectivity_revision_.load(); failure_capture.status = state.status;
        failure_capture.cancellation_generation = diagnostic_cancel_generation_.load(); failure_capture.credential_generation = state.generation;
        failure_capture.memory_count = state.memory_count; failure_capture.remembered = state.remembered;
        failure_capture.user_paused = state.pause_reason == HoldReason::userPaused;
        active_failure_context_ = std::move(failure_capture);
        struct ClearContext {
            std::optional<records::SampleContext>& value;
            std::optional<FailureCapture>& failure;
            ~ClearContext() { value.reset(); failure.reset(); }
        } clear{active_report_context_, active_failure_context_};
        job();
        if (active_failure_context_) record_operation_failure(*active_failure_context_);
    };
    if (!automatic) {
        if (!memory_verification_pending() && memory_verification_message_ != "memory.verification.writeFailed" &&
            !memory_verification_message_.starts_with("campus.verify.")) clear_memory_verification_message();
        manual_queued_ = true;
        jobs_.push_back([this, job = std::move(stamped_job)]() mutable {
            struct ClearPending {
                std::atomic<bool>& value;
                ~ClearPending() { value = false; }
            } clear{manual_queued_};
            job();
        });
    } else jobs_.push_back(std::move(stamped_job));
    worker_cv_.notify_all();
    return true;
}

void AppController::seal_fresh_report_environment(const win::NetworkSnapshot& network, double captured_at) {
    if (!active_report_context_) return;
    auto& environment = active_report_context_->environment;
    environment.refreshing = false;
    environment.connection = network.access;
    environment.tun = network.tunnel_known ? (network.path.tunnel ? "detected" : "notDetected") : "unknown";
    // The fresh attachment capture did not reread Windows proxy settings.
    environment.system_proxy = "unknown";
    const auto state = engine_->snapshot();
    active_report_epoch_ = state.epoch;
    if (active_failure_context_) {
        active_failure_context_->context.environment = environment;
        active_failure_context_->context.physical_network = physical::anonymous_diagnostics(network.physical_diagnostics);
        active_failure_context_->context.physical_collected_at = captured_at;
        active_failure_context_->epoch = active_report_epoch_;
        active_failure_context_->discovery_revision = state.discovery_revision;
        active_failure_context_->memory_count = state.memory_count;
        active_failure_context_->remembered = state.remembered;
        active_failure_context_->status = state.status;
    }
}

AppController::FailureCapture AppController::capture_failure_context(bool automatic) const {
    FailureCapture capture;
    const auto state = engine_->snapshot();
    auto& c = capture.context;
    c.started_at = wall_now(); c.operation = automatic ? "automatic" : "manual";
    c.monitoring_mode = std::string(to_string(state.mode)); c.area = std::string(to_string(state.area));
    c.environment.refreshing = network_refresh_pending_ || !diagnostic_network_fresh_;
    if (!c.environment.refreshing) {
        c.environment.connection = network_.access; c.environment.system_proxy = proxy_.state;
        c.environment.tun = network_.tunnel_known ? (network_.path.tunnel ? "detected" : "notDetected") : "unknown";
        if (network_.path.requires_physical_binding) {
            c.physical_network = physical::anonymous_diagnostics(network_.physical_diagnostics);
            c.physical_collected_at = network_observed_at_;
        }
    }
    capture.epoch = state.epoch; capture.discovery_revision = state.discovery_revision;
    capture.report_revision = connectivity_revision_.load(); capture.status = state.status;
    capture.cancellation_generation = diagnostic_cancel_generation_.load(); capture.credential_generation = state.generation;
    capture.memory_count = state.memory_count; capture.remembered = state.remembered;
    capture.user_paused = state.pause_reason == HoldReason::userPaused;
    return capture;
}

void AppController::record_operation_failure(const FailureCapture& capture, const char* explicit_result) {
    const auto state = engine_->snapshot(); // called only after engine work returned, never inside a core hook
    if (shutting_down_ || state.busy || state.sleeping || state.status == EngineStatus::cancelled || state.epoch != capture.epoch ||
        diagnostic_cancel_generation_ != capture.cancellation_generation || state.generation != capture.credential_generation ||
        state.memory_count != capture.memory_count || state.remembered != capture.remembered ||
        (state.pause_reason == HoldReason::userPaused) != capture.user_paused ||
        to_string(state.mode) != capture.context.monitoring_mode || to_string(state.area) != capture.context.area) return;
    const bool fresh_discovery = state.discovery_revision != capture.discovery_revision && state.last_discovery.has_value();
    if (!explicit_result && connectivity_revision_ == capture.report_revision && !fresh_discovery && state.status == capture.status) return;
    const bool campus_failed = fresh_discovery && (state.automatic_campus_verification_failed ||
        state.required_campus_verification == Discovery::unconfirmed);
    if (!explicit_result && !campus_failed) {
        switch (state.status) {
            case EngineStatus::offline: case EngineStatus::confirming: case EngineStatus::paused: case EngineStatus::coolingDown:
            case EngineStatus::unconfirmed: case EngineStatus::needsTrust: case EngineStatus::needsAccount: case EngineStatus::accepted:
            case EngineStatus::rejected: case EngineStatus::uncertain: case EngineStatus::security: case EngineStatus::unavailable: case EngineStatus::noNetwork: break;
            default: return;
        }
    }
    auto c = capture.context;
    c.collected_at = wall_now(); c.status = std::string(to_string(state.status));
    c.result = explicit_result ? explicit_result : campus_failed ? "verificationFailed" : "operationFailed";
    c.kind = c.result == "verificationFailed" ? "campusVerification" : "connection";
    if (fresh_discovery) {
        c.campus_state = state.required_campus_verification == Discovery::confirmed ? "confirmed" :
            state.required_campus_verification == Discovery::unconfirmed ? "unconfirmed" : "notChecked";
        if (c.campus_state == "unconfirmed") c.campus_issue = state.last_discovery->issue;
    }
    c.recovery.state = std::string(to_string(state.auto_login)); c.recovery.read_only = state.read_only;
    c.recovery.paused = state.paused; c.recovery.network_available = state.network_available;
    c.recovery.network_remembered = state.remembered; c.recovery.session_trusted = state.trusted;
    c.recovery.cooldown_remaining = state.cooldown_remaining;
    for (const auto& hold : state.holds)
        if ((state.area == CampusArea::automatic && (hold.applies(CampusArea::teaching, state.generation, state.epoch) ||
            hold.applies(CampusArea::dormitory, state.generation, state.epoch))) || hold.applies(state.area, state.generation, state.epoch))
            c.recovery.hold_reasons.push_back(hold.reason);
    const auto time = c.collected_at;
    const auto message = std::string(copy::text(c.kind == "campusVerification" ? "event.campusVerificationFailure" : "event.diagnosticsFailure"));
    { std::lock_guard lock(inbox_mutex_); inbox_events_.push_back(records::Event{time, message, true, std::move(c)}); }
    post_changed();
}

void AppController::schedule_auto(int seconds) {
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false;
    next_due_.reset();
    auto_timer_retry_deadline_.reset();
    if (settings_.mode == MonitoringMode::manual) return;
    const int delay = std::max(0, seconds);
    // Win32 timers clamp below USER_TIMER_MINIMUM. A zero-delay fast round must not
    // silently become a one/five-second wait, and a nonzero round keeps its exact policy delay.
    arm_auto_timer(wall_now() + delay);
}

void AppController::arm_auto_timer(double deadline) {
    const auto remaining = std::ceil(std::max(0.0, deadline - wall_now()) * 1000.0);
    const UINT milliseconds = static_cast<UINT>(std::clamp(remaining, 1.0, static_cast<double>(USER_TIMER_MAXIMUM)));
    auto_armed_ = auto_timer_arm_(milliseconds);
    if (auto_armed_) {
        next_due_ = deadline;
        auto_timer_retry_deadline_.reset();
    } else {
        next_due_.reset();
        auto_timer_retry_deadline_ = deadline;
    }
}

void AppController::retry_auto_timer() {
    if (!auto_timer_retry_deadline_ || shutting_down_ || engine_busy_ || campus_verification_schedule_ ||
        settings_.mode == MonitoringMode::manual || engine_->snapshot().sleeping) return;
    // The existing maintenance pulse retries timer allocation, never a network operation.
    // Preserve the original deadline rather than restarting or advancing recovery.
    arm_auto_timer(*auto_timer_retry_deadline_);
}

void AppController::on_changed() {
    posted_ = false;
    drain_diagnostics_export();
    std::vector<records::Event> events;
    std::vector<CompletedReport> reports;
    std::optional<RememberFeedback> remember_result;
    std::optional<CampusFeedback> campus_result;
    std::optional<ManualConnectionCapture> manual_connection;
    bool refresh_labels = false;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        events.swap(inbox_events_);
        reports.swap(inbox_reports_);
        remember_result.swap(inbox_remember_result_);
        campus_result.swap(inbox_campus_result_);
        manual_connection.swap(inbox_manual_connection_);
        refresh_labels = std::exchange(inbox_labels_refresh_, false);
    }
    if (manual_connection && manual_connection_pending_ &&
        manual_connection->generation == manual_connection_generation_ && !shutting_down_) {
        if (engine_busy_ || manual_queued_) {
            // The worker publishes the capture before leaving its queue slot.
            // Keep the result until that slot is released; never queue credentials behind it.
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_manual_connection_ = std::move(manual_connection);
        } else {
            manual_connection_pending_ = false;
            if (!manual_connection->network) {
                identification_feedback_ = "env.identification.readFailed";
                notice_ = "env.identification.readFailed";
            } else {
                if (notice_ == "env.identification.readFailed") notice_.clear();
                KillTimer(window_, kTimerNetwork);
                network_refresh_timer_armed_ = false;
                refresh_proxy();
                apply_network_snapshot(std::move(*manual_connection->network), manual_connection->captured_at);
                const auto generation = manual_connection->generation;
                const bool force = manual_connection->force;
                const auto confirmed_epoch = manual_connection->confirmed_epoch;
                const auto confirmed_report = manual_connection->confirmed_report;
                // The confirmation belongs to the old physical context, not a newly attached network.
                if (!force || (engine_->snapshot().epoch == confirmed_epoch && connectivity_revision_ == confirmed_report &&
                    force_login_available(engine_->snapshot()))) {
                    manual_admission_queued_ = true;
                    const bool admitted = run_engine([this, generation, force, confirmed_epoch, confirmed_report] {
                        struct ClearAdmission {
                            std::atomic<bool>& pending;
                            ~ClearAdmission() { pending = false; }
                        } clear{manual_admission_queued_};
                        if (generation != manual_connection_generation_ || shutting_down_) return;
                        if (force && (engine_->snapshot().epoch != confirmed_epoch || connectivity_revision_ != confirmed_report ||
                            !force_login_available(engine_->snapshot()))) return;
                        refresh_system_connectivity();
                        if (force) engine_->connect_even_if_online();
                        else engine_->connect();
                    });
                    if (!admitted) manual_admission_queued_ = false;
                }
            }
        }
    }
    if (refresh_labels) {
        std::string text;
        if (runtime_->read(L"network-labels.json", text) == win::Storage::ReadStatus::ok)
            if (const auto decoded = NetworkLabels::decode(text)) network_labels_ = *decoded;
    }
    if (campus_result && campus_result->generation == memory_verification_generation_) {
        if (campus_result->epoch != engine_->snapshot().epoch)
            invalidate_memory_verification("campus.verify.changed");
        else if (campus_result->discovery_revision != engine_->snapshot().discovery_revision)
            clear_memory_verification_message();
        else if (network_refresh_pending_) {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_campus_result_ = campus_result;
        } else {
            const char* key = "campus.verify.failed";
            switch (campus_result->result) {
                case ConnectionEngine::CampusVerificationResult::confirmed: key = "campus.verify.confirmed"; break;
                case ConnectionEngine::CampusVerificationResult::cancelled: key = "campus.verify.cancelled"; break;
                case ConnectionEngine::CampusVerificationResult::busy: key = "memory.verification.busy"; break;
                case ConnectionEngine::CampusVerificationResult::unavailable: key = "campus.verify.disconnected"; break;
                case ConnectionEngine::CampusVerificationResult::needsProof: break;
            }
            set_memory_verification_message(key);
        }
    }
    sync_memory_verification_epoch();
    if (remember_result && remember_result->generation == memory_verification_generation_) {
        if (remember_result->epoch != engine_->snapshot().epoch) invalidate_memory_verification("memory.verification.changed");
        else if (network_refresh_pending_) {
            // Wait for the debounced physical snapshot before showing success for a path
            // that might just have changed. A confirmed epoch change discards this result.
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_remember_result_ = remember_result;
            set_memory_verification_message("memory.verification.pending");
        } else {
            const char* key = "memory.verification.needsProof";
            switch (remember_result->result) {
                case ConnectionEngine::RememberResult::remembered: key = "memory.verification.remembered"; break;
                case ConnectionEngine::RememberResult::failed: key = "memory.verification.writeFailed"; break;
                case ConnectionEngine::RememberResult::unreliable: key = "memory.verification.unreliable"; break;
                case ConnectionEngine::RememberResult::readOnly: key = "memory.verification.readOnly"; break;
                case ConnectionEngine::RememberResult::unreadable: key = "memory.verification.unreadable"; break;
                case ConnectionEngine::RememberResult::nameConflict: key = "memory.verification.nameConflict"; break;
                case ConnectionEngine::RememberResult::busy: key = "memory.verification.busy"; break;
                case ConnectionEngine::RememberResult::cancelled: key = "memory.verification.cancelled"; break;
                case ConnectionEngine::RememberResult::needsProof: break;
            }
            set_memory_verification_message(key);
            memory_verification_merge_ids_.clear();
        }
    }
    refresh_network_labels();
    for (auto& event : events) {
        const bool failure = event.failure || is_failure_event(event.message);
        logs_dirty_ |= events_.append(event.time, std::move(event.message), failure, event.context);
    }
    for (const auto& report : reports) {
        health_.append(report.sample);
        if (report.epoch == engine_->snapshot().epoch) last_checked_ = report.sample.time;
        logs_dirty_ = true;
    }
    if (logs_dirty_ && !read_only_) SetTimer(window_, kTimerPersist, 1500, nullptr);
    restore_campus_verification_schedule();
    if (!campus_verification_schedule_ && !engine_busy_ && !manual_queued_ && !manual_connection_pending_ &&
        !auto_armed_ && !auto_timer_retry_deadline_ && settings_.mode != MonitoringMode::manual &&
        !engine_->snapshot().sleeping)
        schedule_auto(engine_->next_check_delay());
}

void AppController::on_network_signal() {
    invalidate_manual_connection(false);
    ++identification_generation_;
    invalidate_proxy_assistant_round();
    clear_system_connectivity(settings_.mode != MonitoringMode::manual && !engine_->snapshot().sleeping);
    // Reject new admissions too: proofs obtained during debounce must not bind to the old path.
    network_refresh_pending_ = true;
    // TUN/route notifications are not proof that the selected physical attachment changed.
    // The engine and source-bound transport revoke only after capturing that attachment.
    internet_service_.invalidate_website_path(); cancel_compat();
    // Coalesce this burst without moving its first deadline: sustained route/TUN
    // notifications must not indefinitely postpone capturing the physical identity.
    // If allocation fails, keep admissions gated and retry scheduling on the next signal.
    if (!network_refresh_timer_armed_)
        network_refresh_timer_armed_ = SetTimer(window_, kTimerNetwork, 300, nullptr) != 0;
}

void AppController::on_timer(UINT_PTR id) {
    switch (id) {
        case kTimerAuto:
            if (auto_timer_retry_deadline_) return;
            if (manual_connection_pending_) return;
            KillTimer(window_, kTimerAuto);
            auto_armed_ = false;
            next_due_.reset();
            if (engine_busy_) return;  // on_changed re-arms once the current operation ends
            run_engine([this] {
                // The IP/route watchers may not fire for an association change. Capture on the
                // worker, before any probe or credential decision, to keep the UI responsive.
                const auto fresh = runtime_->capture_network();
                const auto captured_at = wall_now();
                const auto ssid = fresh.current_label && fresh.current_label->kind == "wifi"
                    ? fresh.current_label->ssid : std::nullopt;
                const bool association_changed = !fresh.path.requires_physical_binding && last_auto_wifi_ssid_ && ssid && *last_auto_wifi_ssid_ != *ssid;
                const bool path_changed = last_auto_identity_ != fresh.path.identity;
                const auto previous_epoch = engine_->snapshot().epoch;
                proxy_physical_link_active_ = fresh.path.link_active;
                if (association_changed) {
                    // SSID is revocation evidence only. It is never a remembered-network key.
                    engine_->receive_path(PathSnapshot{});
                }
                engine_->receive_path(fresh.path);
                if (engine_->snapshot().epoch != previous_epoch) { internet_service_.invalidate(); clear_system_connectivity(true); }
                if (ssid) last_auto_wifi_ssid_ = ssid;
                last_auto_identity_ = fresh.path.identity;
                if (association_changed || path_changed) PostMessageW(window_, kMsgNetwork, 0, 0);
                seal_fresh_report_environment(fresh, captured_at);
                refresh_system_connectivity();
                engine_->automatic_round();
            }, true);
            break;
        case kTimerNetwork: {
            KillTimer(window_, kTimerNetwork);
            network_refresh_timer_armed_ = false;
            const auto previous_epoch = engine_->snapshot().epoch;
            refresh_proxy();
            refresh_network();
            if (settings_.mode != MonitoringMode::manual && !engine_->snapshot().sleeping) {
                // Network signals invalidate system-route evidence independently of the
                // physical authentication path. Recheck only an existing display request;
                // never turn the first notification or manual mode into automatic traffic.
                refresh_system_connectivity(true);
            } else {
                std::lock_guard<std::mutex> lock(system_probe_mutex_);
                system_probe_recheck_pending_ = false;
            }
            // Virtual-route notifications can repeat throughout a TUN outage. Only a
            // new physical epoch replaces an armed deadline; an unchanged attachment
            // keeps its original cadence, including longer protected/verification waits.
            const auto current = engine_->snapshot();
            if (settings_.mode != MonitoringMode::manual && !engine_busy_ && !current.sleeping &&
                !campus_verification_schedule_ && ((!auto_armed_ && !auto_timer_retry_deadline_) || current.epoch != previous_epoch))
                schedule_auto(engine_->next_check_delay());
            post_changed();
            break;
        }
        case kTimerPersist:
            KillTimer(window_, kTimerPersist);
            persist_logs();
            break;
        case kTimerWake:
            KillTimer(window_, kTimerWake);
            refresh_proxy();
            refresh_network();
            if (settings_.mode != MonitoringMode::manual) schedule_auto(1);
            post_changed();
            break;
        case kTimerWiFi: {
            retry_auto_timer();
            const auto previous_status = wifi_status();
            const auto previous_notice = notice_;
            update_wifi_context();
            bool ready = false;
            { std::lock_guard<std::mutex> lock(wifi_mutex_); ready = wifi_schedule_.ready(wall_now()); }
            // The worker rechecks admission in begin(); this only avoids empty jobs
            // while disabled, connected, paused or waiting for the retry deadline.
            if (ready && !engine_busy_) run_engine([this] { run_wifi_recovery(); }, true);
            if (previous_status != wifi_status() || previous_notice != notice_) post_changed();
            break;
        }
        default: break;
    }
}

void AppController::on_suspend() {
    diagnostic_network_fresh_ = false;
    invalidate_manual_connection();
    ++identification_generation_;
    invalidate_proxy_assistant_round(true);
    const bool remembering = memory_verification_pending();
    clear_system_connectivity();
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.cancel(); }
    internet_service_.invalidate();
    engine_->set_sleeping(true);  // cancels work and revokes network trust
    if (remembering) invalidate_memory_verification("memory.verification.changed");
    else clear_memory_verification_message();
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false;
    next_due_.reset();
    auto_timer_retry_deadline_.reset();
    cancel_compat();
}

void AppController::on_resume() {
    diagnostic_network_fresh_ = false;
    engine_->set_sleeping(false);
    SetTimer(window_, kTimerWake, 1000, nullptr);  // fresh physical binding, then policy controls retries
}

void AppController::refresh_network() {
    invalidate_manual_connection(false);
    apply_network_snapshot(runtime_->capture_network());
}

void AppController::apply_network_snapshot(win::NetworkSnapshot latest, std::optional<double> captured_at) {
    ++identification_generation_;
    if (latest.path.identity != network_.path.identity || latest.path.usable != network_.path.usable ||
        latest.path.tunnel != network_.path.tunnel) { internet_service_.invalidate_website_path(); cancel_compat(); }
    network_ = std::move(latest);
    network_observed_at_ = captured_at.value_or(wall_now());
    proxy_physical_link_active_ = network_.path.link_active;
    const auto previous_epoch = engine_->snapshot().epoch;
    engine_->receive_path(network_.path);
    sync_proxy_assistant_context(engine_->snapshot());
    if (engine_->snapshot().epoch != previous_epoch) {
        identification_feedback_.clear();
        internet_service_.invalidate();
        clear_system_connectivity(network_refresh_pending_ && settings_.mode != MonitoringMode::manual && !engine_->snapshot().sleeping);
    }
    sync_memory_verification_epoch();
    diagnostic_network_fresh_ = true;
    network_refresh_pending_ = false;
    refresh_network_labels();
    update_wifi_context();
}

bool AppController::save_network_labels(const NetworkLabels& labels) {
    if (read_only_ || !network_labels_writable_) return false;
    std::string existing;
    const auto status = runtime_->read(L"network-labels.json", existing);
    if (status == win::Storage::ReadStatus::failed ||
        (status == win::Storage::ReadStatus::ok && !NetworkLabels::decode(existing))) {
        network_labels_writable_ = false;
        return false;
    }
    return runtime_->write(L"network-labels.json", labels.encode());
}

bool AppController::reload_network_labels() {
    std::string existing;
    const auto status = runtime_->read(L"network-labels.json", existing);
    const auto decoded = status == win::Storage::ReadStatus::missing ? std::optional<NetworkLabels>(NetworkLabels{}) :
        status == win::Storage::ReadStatus::ok ? NetworkLabels::decode(existing) : std::nullopt;
    if (!decoded) { network_labels_writable_ = false; return false; }
    network_labels_ = *decoded;
    return true;
}

void AppController::refresh_network_labels() {
    // A merge writes its metadata on the worker before committing memory. Keep the
    // entire UI read/derive/write transaction outside any queued or active job, and
    // read the committed sidecar even when its result message has not arrived yet.
    std::lock_guard<std::mutex> gate(worker_mutex_);
    if (engine_busy_ || !jobs_.empty() || !reload_network_labels()) return;
    const auto snapshot = engine_->snapshot();
    if (snapshot.memory_unreadable) return;
    const auto records = engine_->remembered_networks();
    std::set<std::string> ids;
    for (const auto& record : records) ids.insert(record.id);
    auto next = network_labels_;
    bool changed = next.retain(ids);
    // Labels must follow the authentication attachment, rather than a different system-primary NIC.
    std::optional<NetworkLabel> selected_label;
    if (snapshot.authentication_route) {
        selected_label = runtime_->network_label(*snapshot.authentication_route);
    } else if (!network_.path.requires_physical_binding) selected_label = network_.current_label;
    if (!network_refresh_pending_ && selected_label) {
        for (const auto& id : engine_->current_remembered_ids())
            changed = next.update_automatic(id, *selected_label) || changed;
    }
    if (changed) {
        // Forgotten metadata disappears in memory even if a sidecar write fails.
        network_labels_ = next;
        save_network_labels(next);
    }
    update_wifi_context();
}

std::string AppController::network_display_name(const std::string& id) const {
    return network_name(id);
}

std::optional<std::string> AppController::current_network_record_id() const {
    if (network_refresh_pending_ || network_.path.physical_candidates.size() != 1) return std::nullopt;
    const auto reliable = [](const EngineSnapshot& state) {
        return state.network_available && !state.sleeping && !state.memory_unreadable && state.remembered &&
            !state.multiple_networks && state.authentication_route && state.authentication_route->descriptor.reliable();
    };
    const auto before = engine_->snapshot();
    if (!reliable(before) || network_.path.physical_candidates.front() != *before.authentication_route) return std::nullopt;
    const auto matches = engine_->current_remembered_ids();
    if (matches.size() != 1) return std::nullopt;
    const auto after = engine_->snapshot();
    if (!reliable(after) || before.epoch != after.epoch || before.authentication_route != after.authentication_route)
        return std::nullopt;
    return matches.front();
}

std::string AppController::network_kind(const std::string& id) const {
    const auto found = network_labels_.entries().find(id);
    return found == network_labels_.entries().end() ? "other" : found->second.kind;
}

std::string AppController::authentication_target_name() const {
    const auto state = snapshot();
    if (!state.authentication_route) return {};
    const auto ids = engine_->current_remembered_ids();
    if (state.remembered && ids.size() == 1 && network_name_known(ids.front())) return network_display_name(ids.front());
    if (network_.path.physical_candidates.size() == 1 && network_.current_label &&
        network_.path.physical_candidates.front() == *state.authentication_route && !network_.current_label->display_name.empty())
        return network_.current_label->display_name;
    return std::string(copy::text(state.authentication_route->descriptor.kind == "wifi" ? "memory.item.wifi" : "memory.item.ethernet"));
}

std::string AppController::physical_system_primary_name() const {
    const auto& candidates = network_.path.physical_candidates;
    const auto primary = std::find_if(candidates.begin(), candidates.end(), [](const auto& route) { return route.is_system_primary; });
    if (primary == candidates.end()) return {};
    return primary->display_name.empty()
        ? std::string(copy::text(primary->descriptor.kind == "wifi" ? "memory.item.wifi" : "memory.item.ethernet"))
        : primary->display_name;
}

std::vector<std::string> AppController::physical_connection_names() const {
    std::vector<std::string> result;
    for (const auto& route : network_.path.physical_candidates)
        result.push_back(route.display_name.empty()
            ? std::string(copy::text(route.descriptor.kind == "wifi" ? "memory.item.wifi" : "memory.item.ethernet"))
            : route.display_name);
    return result;
}

bool AppController::physical_identification_needed() const {
    const auto state = snapshot();
    return network_.path.requires_physical_binding && !state.sleeping && !state.authentication_route &&
        (state.network_available || network_.path.link_active || network_.other_physical_link_active ||
         network_.physical_diagnostics.interface_count || !network_.physical_diagnostics.rejections.empty());
}

std::string AppController::physical_identification_detail() const {
    return records::physical_identification_key(network_.physical_diagnostics, snapshot().authentication_route.has_value());
}

void AppController::refresh_physical_identification() {
    // This action updates anonymous observations only. Applying a new path can
    // arm recovery, so normal watchers remain responsible for path admission.
    if (shutting_down_ || snapshot().sleeping) return;
    const auto epoch = snapshot().epoch;
    const auto generation = ++identification_generation_;
    try {
        auto diagnostics = runtime_->capture_physical_diagnostics();
        if (shutting_down_ || generation != identification_generation_) return;
        if (snapshot().sleeping || snapshot().epoch != epoch) identification_feedback_ = "env.refreshChanged";
        else {
            network_.physical_diagnostics = physical::anonymous_diagnostics(diagnostics);
            identification_feedback_ = "env.identification.refreshed";
        }
    } catch (...) {
        if (shutting_down_ || generation != identification_generation_) return;
        identification_feedback_ = "env.identification.readFailed";
    }
    post_changed();
}

bool AppController::rename_network(const std::string& id, const std::string& name) {
    std::lock_guard<std::mutex> gate(worker_mutex_);
    if (engine_busy_ || !jobs_.empty() || !reload_network_labels()) return false;
    if (read_only_ || !network_labels_writable_) return false;
    const auto records = engine_->remembered_networks();
    if (std::none_of(records.begin(), records.end(), [&](const MemoryRecord& r) { return r.id == id; })) return false;
    auto next = network_labels_;
    if (!next.rename(id, name)) return NetworkLabels::clean_name(name) == network_labels_.display_name(id);
    if (!save_network_labels(next)) { notice_ = "error.storage"; return false; }
    network_labels_ = std::move(next);
    return true;
}

wifi::Preferences AppController::wifi_preferences() const {
    std::lock_guard<std::mutex> lock(wifi_mutex_); return wifi_preferences_;
}
std::vector<wifi::Network> AppController::wifi_candidates() const {
    std::lock_guard<std::mutex> lock(wifi_mutex_); return wifi_candidates_;
}
std::string AppController::wifi_status() const {
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    return wifi_unreadable_ ? "unavailable" : wifi_schedule_.status();
}
bool AppController::wifi_selection_required() const {
    if (read_only_) return false;
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    return !wifi_unreadable_ && wifi_preferences_.auto_join &&
        std::none_of(wifi_candidates_.begin(), wifi_candidates_.end(),
            [&](const auto& network) { return wifi_preferences_.selected(network); });
}
bool AppController::wifi_unreadable() const { std::lock_guard<std::mutex> lock(wifi_mutex_); return wifi_unreadable_; }
bool AppController::wifi_selected(const std::string& id) const {
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    const auto found = std::find_if(wifi_candidates_.begin(), wifi_candidates_.end(), [&](const auto& n) { return n.id == id; });
    return !wifi_unreadable_ && found != wifi_candidates_.end() && wifi_preferences_.selected(*found);
}
std::string AppController::wifi_name(const std::string& id) const {
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    const auto found = std::find_if(wifi_candidates_.begin(), wifi_candidates_.end(), [&](const auto& n) { return n.id == id; });
    return found == wifi_candidates_.end() ? "" : found->ssid;
}
bool AppController::is_wifi_record(const std::string& id) const {
    const auto records = engine_->remembered_networks();
    const auto label = network_labels_.entries().find(id);
    return label != network_labels_.entries().end() && label->second.kind == "wifi" &&
        std::any_of(records.begin(), records.end(), [&](const auto& record) { return record.id == id; });
}
bool AppController::can_edit_wifi_name(const std::string& id) const {
    if (!is_wifi_record(id) || wifi_unreadable()) return false;
    const auto& label = network_labels_.entries().at(id);
    return !label.ssid || wifi_preferences().manual_names.count(id) != 0;
}
void AppController::update_wifi_context() {
    const auto state = engine_->snapshot();
    const auto records = engine_->remembered_networks();
    // Migrate the old explicit allowlist only once the remembered context is nonempty.
    // A failed write keeps the conservative choice in RAM; damaged originals are never overwritten.
    auto migrated = wifi_preferences();
    if (!read_only_ && settings_writable_ && !wifi_unreadable() && migrated.migrate(wifi::candidates(records, network_labels_, migrated))) {
        { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_preferences_ = migrated; }
        std::string original;
        const auto status = runtime_->read(L"wifi-recovery.json", original);
        if (status == win::Storage::ReadStatus::failed || (status == win::Storage::ReadStatus::ok && !wifi::Preferences::decode(original))) {
            std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_unreadable_ = true;
        } else if (const auto encoded = migrated.encode(); !encoded || !runtime_->write(L"wifi-recovery.json", *encoded))
            notice_ = "wifiRecovery.revokeSaveFailed";
    }
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    wifi_candidates_ = wifi::candidates(records, network_labels_, wifi_preferences_);
    std::vector<wifi::Network> selected;
    for (const auto& network : wifi_candidates_) if (wifi_preferences_.selected(network)) selected.push_back(network);
    const bool physical = network_.path.link_active || network_.other_physical_link_active || network_.access == "unknown";
    wifi_schedule_.update(std::move(selected), !read_only_ && settings_writable_ && !wifi_unreadable_ && wifi_preferences_.auto_join,
        !read_only_ && settings_writable_ && !wifi_unreadable_ && wifi_preferences_.power_on,
        !read_only_ && settings_writable_ && state.mode == MonitoringMode::recover && state.has_saved && !state.paused && state.holds.empty(),
        physical, state.sleeping);
}
bool AppController::save_wifi_preferences(wifi::Preferences next, bool revoking, bool merge_preflight) {
    if (read_only_) return false;
    if (revoking && !merge_preflight) {
        const auto previous = wifi_preferences();
        const bool newly_excluded = std::any_of(memory_verification_merge_ids_.begin(), memory_verification_merge_ids_.end(),
            [&](const auto& id) { return next.excluded.count(id) && !previous.excluded.count(id); });
        const bool disabled_finding = (previous.auto_join && !next.auto_join) || (previous.power_on && !next.power_on);
        if ((newly_excluded || disabled_finding) && (engine_->snapshot().verifying_memory ||
            (!campus_service_only_verification_ && memory_verification_pending()))) {
            if (newly_excluded) {
                const auto exclusions = next.excluded;
                next.excluded.insert(memory_verification_merge_ids_.begin(), memory_verification_merge_ids_.end());
                if (next.excluded.size() > 64) { next.excluded = exclusions; next.auto_join = false; }
            }
            // Also retain the opt-out if the worker committed just before this cancellation.
            engine_->cancel();
            invalidate_memory_verification("memory.verification.cancelled");
        }
    }
    const auto encoded = next.encode(); if (!encoded) return false;
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        wifi_schedule_.cancel();
        // An opt-out takes effect immediately even if a later disk write cannot complete.
        if (revoking) wifi_preferences_ = next;
        if (wifi_unreadable_) return false;
    }
    std::string existing;
    const auto status = runtime_->read(L"wifi-recovery.json", existing);
    if (status == win::Storage::ReadStatus::failed || (status == win::Storage::ReadStatus::ok && !wifi::Preferences::decode(existing))) {
        { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_unreadable_ = true; }
        notice_ = "wifiRecovery.unreadable"; update_wifi_context(); return false;
    }
    if (!runtime_->write(L"wifi-recovery.json", *encoded)) {
        notice_ = revoking ? "wifiRecovery.revokeSaveFailed" : "wifiRecovery.saveFailed";
        update_wifi_context(); return false;
    }
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_preferences_ = next; }
    update_wifi_context(); post_changed(); return true;
}
bool AppController::set_wifi_auto_join(bool enabled) {
    auto next = wifi_preferences(); next.auto_join = enabled; return save_wifi_preferences(next, !enabled);
}
bool AppController::set_wifi_power_on(bool enabled) {
    auto next = wifi_preferences(); next.power_on = enabled; return save_wifi_preferences(next, !enabled);
}
bool AppController::set_wifi_selected(const std::string& id, bool enabled) {
    const bool known = is_wifi_record(id);
    const bool pending_merged_opt_out = !known && !enabled && memory_verification_pending() &&
        std::find(memory_verification_merge_ids_.begin(), memory_verification_merge_ids_.end(), id) != memory_verification_merge_ids_.end() &&
        std::any_of(memory_verification_merge_ids_.begin(), memory_verification_merge_ids_.end(),
            [&](const auto& candidate) { return is_wifi_record(candidate); });
    if (!known && !pending_merged_opt_out) return false;
    auto next = wifi_preferences();
    if (pending_merged_opt_out) {
        const auto exclusions = next.excluded;
        next.excluded.insert(memory_verification_merge_ids_.begin(), memory_verification_merge_ids_.end());
        if (next.excluded.size() > 64) { next.excluded = exclusions; next.auto_join = false; }
        return save_wifi_preferences(next, true);
    }
    if (next.legacy_selections) next.migrate(wifi_candidates());
    if (enabled) next.excluded.erase(id);
    else if (!next.excluded.count(id) && next.excluded.size() >= 64) { next.auto_join = false; return save_wifi_preferences(next, true); }
    else next.excluded.insert(id);
    return save_wifi_preferences(next, !enabled);
}
bool AppController::set_wifi_name(const std::string& id, const std::string& ssid) {
    if (!is_wifi_record(id) || !NetworkLabels::valid_ssid(ssid)) return false;
    auto next = wifi_preferences(); next.manual_names[id] = ssid;
    return save_wifi_preferences(next, false); // helper revokes in-flight association before committing a replacement
}
void AppController::run_wifi_recovery() {
    std::optional<wifi::Schedule::Request> request;
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        request = wifi_schedule_.begin(wall_now());
    }
    if (!request) return;
    post_changed();
    const auto result = runtime_->recover_wifi(request->networks, request->power_on, request->token);
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.finish(result, wall_now()); }
    if (result == wifi::Result::joined || result == wifi::Result::poweredOn || result == wifi::Result::alreadyConnected)
        PostMessageW(window_, kMsgNetwork, 0, 0);
    post_changed();
}

void AppController::refresh_proxy() {
    auto latest = proxy::summarize(proxy::parse_windows(runtime_->read_proxy()));
    if (latest.state != proxy_.state || latest.kinds != proxy_.kinds || latest.campus_bypass != proxy_.campus_bypass) {
        internet_service_.invalidate_website_path(); cancel_compat();
    }
    proxy_ = std::move(latest);
}

// ---------------- commands ----------------
void AppController::refresh_environment() {
    refresh_proxy();
    refresh_network();
    environment_refreshed_ = wall_now();
    environment_refresh_incomplete_ = network_.access.empty() || network_.access == "unknown" ||
        !network_.tunnel_known || proxy_.state == "unknown";
    post_changed();
}

void AppController::check() {
    run_engine([this] { check_with_system_connectivity(); });
}
std::optional<ConnectivityReport> AppController::system_connectivity() const {
    std::lock_guard<std::mutex> lock(system_probe_mutex_); return system_report_;
}
std::optional<double> AppController::system_checked_at() const {
    std::lock_guard<std::mutex> lock(system_probe_mutex_); return system_checked_at_;
}
std::optional<ProbeMode> AppController::system_checked_mode() const {
    std::lock_guard<std::mutex> lock(system_probe_mutex_);
    return system_report_ ? std::optional<ProbeMode>(system_report_->mode) : std::nullopt;
}
bool AppController::system_connectivity_running() const {
    std::lock_guard<std::mutex> lock(system_probe_mutex_); return system_probe_running_ || system_probe_recheck_pending_;
}
void AppController::clear_system_connectivity(bool recheck_after_network_refresh) {
    invalidate_proxy_assistant_round();
    CancelToken stale;
    {
        std::lock_guard<std::mutex> lock(system_probe_mutex_);
        // Keep only the intent to replace a previously requested display check, not
        // its result or token. Explicit cancellation, sleep and shutdown revoke it.
        system_probe_recheck_pending_ = recheck_after_network_refresh && !system_probe_stopping_ && !shutting_down_ &&
            (system_probe_recheck_pending_ || system_probe_running_ || system_report_.has_value() || system_probe_pending_.has_value());
        ++system_probe_generation_; system_report_.reset(); system_checked_at_.reset(); stale = system_probe_token_;
        system_probe_running_ = false;
        system_probe_pending_.reset();
    }
    stale.cancel();
}
void AppController::refresh_system_connectivity(bool recheck_only) {
    CancelToken stale;
    const auto state = engine_->snapshot();
    const ProbeMode mode = state.probe_mode;
    sync_proxy_assistant_context(state);
    {
        std::lock_guard<std::mutex> lock(system_probe_mutex_);
        if (system_probe_stopping_ || shutting_down_ || (recheck_only && !system_probe_recheck_pending_)) return;
        system_probe_recheck_pending_ = false;
        stale = system_probe_token_;
        system_probe_token_ = CancelToken{};
        ++system_probe_generation_;
        system_report_.reset();
        system_checked_at_.reset();
        system_probe_running_ = true;
        // A single pending slot coalesces repeated rounds. Every superseded token is
        // cancelled, and only the newest generation can publish display evidence.
        system_probe_pending_ = SystemProbeRequest{system_probe_token_, system_probe_generation_, mode};
        if (!recheck_only) {
            std::lock_guard<std::mutex> suggestion_lock(proxy_suggestion_mutex_);
            proxy_suggestion_round_.reset();
            if (proxy_suggestion_context_)
                proxy_suggestion_round_ = ProxyAssistantRound{system_probe_generation_, proxy_suggestion_revision_};
        }
    }
    stale.cancel();
    system_probe_cv_.notify_all();
    post_changed();
}
void AppController::check_with_system_connectivity() {
    refresh_system_connectivity();
    engine_->check();
}
void AppController::connect() {
    queue_manual_connection(false);
}
void AppController::force_login() {
    if (read_only_ || settings_.mode == MonitoringMode::observe || manual_connection_pending_) return;
    const auto state = engine_->snapshot();
    const auto report_revision = connectivity_revision_.load();
    const auto context_revision = manual_connection_generation_.load();
    if (!force_login_available(state)) return;
    const auto title = win::widen(copy::text("internet.partial.loginWarning.title"));
    const auto body = win::widen(copy::text("internet.partial.loginWarning.body"));
    // All UI force actions pass here. Cancel, close and any error leave the
    // engine untouched; the explicit continuation still uses the original core guards.
    if (!force_login_confirmation_(window_, title, body)) return;
    if (manual_connection_generation_ != context_revision || engine_->snapshot().epoch != state.epoch || connectivity_revision_ != report_revision ||
        !force_login_available(engine_->snapshot())) return;
    queue_manual_connection(true, state.epoch, report_revision);
}
void AppController::login_once() { connect(); }
void AppController::invalidate_manual_connection(bool diagnostic_boundary) {
    ++manual_connection_generation_;
    if (diagnostic_boundary) ++diagnostic_cancel_generation_;
    manual_connection_pending_ = false;
    manual_admission_queued_ = false;
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    inbox_manual_connection_.reset();
}

void AppController::queue_manual_connection(bool force, std::uint64_t confirmed_epoch, std::uint64_t confirmed_report) {
    if (manual_connection_pending_ || shutting_down_ || engine_->snapshot().sleeping) return;
    const auto generation = ++manual_connection_generation_;
    const bool admitted = run_engine([this, generation, confirmed_epoch, confirmed_report, force] {
        if (generation != manual_connection_generation_ || shutting_down_) return;
        std::optional<win::NetworkSnapshot> latest;
        try { latest = runtime_->capture_network(); } catch (...) {}
        const auto captured_at = wall_now();
        if (generation != manual_connection_generation_ || shutting_down_) return;
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            if (generation != manual_connection_generation_ || shutting_down_) return;
            inbox_manual_connection_ = ManualConnectionCapture{generation, confirmed_epoch, confirmed_report, force, captured_at, std::move(latest)};
        }
        post_changed();
    });
    if (admitted) { manual_connection_pending_ = true; post_changed(); }
}
void AppController::cancel() {
    invalidate_manual_connection();
    ++identification_generation_;
    invalidate_proxy_assistant_round(true);
    const auto state = engine_->snapshot();
    const bool service_only = state.verifying_campus && !state.auto_verifying_campus;
    if (state.verifying_memory || state.verifying_campus || memory_verification_pending())
        invalidate_memory_verification(state.auto_verifying_campus ? "campus.verify.cancelled" : "memory.verification.cancelled");
    if (service_only) { engine_->cancel(); return; }
    clear_system_connectivity();
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.cancel(); }
    engine_->cancel(); internet_service_.invalidate();
}

void AppController::set_mode(MonitoringMode mode) {
    if (read_only_) return;
    if (!settings_writable_ && mode != MonitoringMode::manual) {
        notice_ = "error.settingsUnreadable"; post_changed(); return;
    }
    if (settings_.mode != mode) invalidate_manual_connection();
    if (settings_.mode != mode && engine_->snapshot().verifying_memory)
        invalidate_memory_verification("memory.verification.cancelled");
    settings_.mode = mode;
    if (mode == MonitoringMode::manual) {
        std::lock_guard<std::mutex> lock(system_probe_mutex_);
        system_probe_recheck_pending_ = false;
    }
    engine_->set_mode(mode);
    update_wifi_context();
    save_settings();
    schedule_auto(mode == MonitoringMode::manual ? 0 : 2);
}

void AppController::set_area(CampusArea area) {
    if (read_only_) return;
    if (settings_.area != area) { invalidate_manual_connection(); clear_memory_verification_message(); }
    settings_.area = area;
    engine_->set_area(area);
    save_settings();
}

void AppController::set_probe(ProbeMode mode) {
    // Observe-only may choose a session-local check; save_settings still forbids persistence.
    std::lock_guard<std::mutex> gate(worker_mutex_);
    if (stopping_ || engine_busy_ || manual_connection_pending_ || !jobs_.empty() || engine_->snapshot().busy) return;
    // Selection configures the next check; completed evidence and its time still describe
    // the last request, even when that request used a different scope.
    settings_.probe = mode;
    engine_->set_probe_mode(mode);
    save_settings();
}

bool AppController::save_website_preferences(const WebsitePreferences& preferences) {
    if (read_only_) return false;
    // The queue lock covers disk commit too: a queued authentication job cannot start in the
    // gap between the busy check and applying configuration. No in-flight operation is cancelled.
    std::lock_guard<std::mutex> gate(worker_mutex_);
    if (stopping_ || engine_busy_ || manual_connection_pending_ || !jobs_.empty() || engine_->snapshot().busy) {
        website_configuration_error_ = "internet.settingsBusy";
        return false;
    }
    if (!preferences.encode()) { website_configuration_error_ = "website.error.invalid"; return false; }
    const auto previous = website_preferences_.current();
    const bool saved = website_preferences_.save(preferences, [this](const std::string& data, bool preserve) {
        return runtime_->write_preserving(L"websites.json", data, preserve);
    });
    if (!saved) { website_configuration_error_ = "internet.settingsWriteFailed"; post_changed(); return false; }
    // Runtime and stale-result generation change only after the durable write succeeds.
    const auto& current = website_preferences_.current();
    const auto selected = [](const std::vector<WebsiteTarget>& targets) {
        std::vector<std::pair<std::string, std::string>> rows;
        for (const auto& target : targets) if (target.enabled) rows.emplace_back(target.id, target.url);
        return rows;
    };
    const bool effective_change = previous.follow_system_proxy != current.follow_system_proxy ||
        selected(previous.targets) != selected(current.targets);
    internet_service_.configure(current.targets, current.follow_system_proxy);
    if (effective_change) engine_->reset_partial_confirmation();
    website_configuration_error_.clear();
    post_changed();
    return true;
}

bool AppController::replace_website_targets(const std::vector<WebsiteTarget>& targets, bool select_complete) {
    if (!save_website_preferences({targets, website_follow_system_proxy()})) return false;
    if (select_complete && std::any_of(website_targets().begin(), website_targets().end(), [](const WebsiteTarget& site) { return site.enabled; })) {
        settings_.probe = ProbeMode::complete;
        engine_->set_probe_mode(ProbeMode::complete);
        save_settings();
    }
    post_changed();
    return true;
}

void AppController::set_website_proxy(bool follow_system_proxy) {
    if (follow_system_proxy == website_follow_system_proxy() && !website_preferences_.needs_preservation()) return;
    save_website_preferences({website_targets(), follow_system_proxy});
}

void AppController::pause() { engine_->pause_auto_login(); update_wifi_context(); }

void AppController::resume() {
    engine_->resume_auto_login();
    update_wifi_context();
    if (settings_.mode == MonitoringMode::recover && !engine_busy_) schedule_auto(1);
}

bool AppController::trust() {
    const bool ok = engine_->trust_current_network();
    if (ok && settings_.mode == MonitoringMode::recover && !engine_busy_) schedule_auto(1);
    return ok;
}

void AppController::revoke_trust() {
    if (engine_->snapshot().verifying_memory) invalidate_memory_verification("memory.verification.cancelled");
    engine_->revoke_trust();
}

std::string AppController::memory_verification_message() const {
    const auto state = engine_->snapshot();
    const auto automatic_feedback = [&state]() -> std::string {
        if (state.auto_verifying_campus) return "campus.verify.checking";
        if (state.automatic_campus_verification_pending) return "campus.verify.autoFailed";
        return "";
    };
    if (memory_verification_message_.empty()) return automatic_feedback();
    if (memory_verification_area_ && *memory_verification_area_ != state.area) return automatic_feedback();
    if (memory_verification_epoch_ && *memory_verification_epoch_ != state.epoch)
        return memory_verification_pending() ? (campus_service_only_verification_ ? "campus.verify.changed" : "memory.verification.changed") : automatic_feedback();
    if (!memory_verification_pending() && memory_verification_message_.starts_with("campus.verify.") &&
        memory_verification_discovery_revision_ != state.discovery_revision) return automatic_feedback();
    if ((state.auto_verifying_campus || state.automatic_campus_verification_pending) &&
        !memory_verification_pending() && memory_verification_message_ != "memory.verification.writeFailed")
        return automatic_feedback();
    return memory_verification_message_;
}

void AppController::set_memory_verification_message(std::string key) {
    memory_verification_message_ = std::move(key);
    const auto state = engine_->snapshot();
    memory_verification_epoch_ = state.epoch;
    memory_verification_area_ = state.area;
    memory_verification_discovery_revision_ = state.discovery_revision;
}

void AppController::clear_memory_verification_message() {
    ++memory_verification_generation_;
    memory_verification_message_.clear();
    memory_verification_epoch_.reset();
    memory_verification_area_.reset();
    memory_verification_merge_ids_.clear();
    campus_service_only_verification_ = false;
}

bool AppController::memory_verification_pending() const {
    return memory_verification_message_ == "memory.verification.checking" ||
        memory_verification_message_ == "memory.verification.pending" || memory_verification_message_ == "campus.verify.checking";
}

void AppController::invalidate_memory_verification(const char* key) {
    ++memory_verification_generation_;
    if (campus_service_only_verification_) {
        if (std::string_view(key) == "memory.verification.changed") key = "campus.verify.changed";
        else if (std::string_view(key) == "memory.verification.cancelled") key = "campus.verify.cancelled";
    }
    set_memory_verification_message(key);
    memory_verification_merge_ids_.clear();
}

void AppController::sync_memory_verification_epoch() {
    if (memory_verification_message_.empty()) return;
    const auto state = engine_->snapshot();
    if (memory_verification_area_ && *memory_verification_area_ != state.area) {
        clear_memory_verification_message();
    } else if (memory_verification_epoch_ && *memory_verification_epoch_ != state.epoch) {
        if (memory_verification_pending()) invalidate_memory_verification("memory.verification.changed");
        else clear_memory_verification_message();
    } else if (!memory_verification_pending() && memory_verification_message_.starts_with("campus.verify.") &&
        memory_verification_discovery_revision_ != state.discovery_revision) {
        clear_memory_verification_message();
    }
}

bool AppController::remember_network() {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    const auto reject = [this](const char* key) {
        set_memory_verification_message(key);
        post_changed();
        return false;
    };
    if (stopping_ || shutting_down_) return reject("memory.verification.cancelled");
    if (read_only_) return reject("memory.verification.readOnly");
    if (network_refresh_pending_) return reject("memory.verification.pending");
    const auto state = engine_->snapshot();
    if (state.verifying_memory) return reject("memory.verification.checking");
    if (engine_busy_ || manual_connection_pending_ || !jobs_.empty() || state.busy) return reject("memory.verification.busy");
    if (!reload_network_labels()) return reject("memory.verification.writeFailed");
    if (state.memory_unreadable) return reject("memory.verification.unreadable");
    if (!state.network_available || state.sleeping) return reject("memory.verification.unavailable");
    if (state.remember_blocker == "unreliable") return reject("memory.verification.unreliable");
    if (!state.can_remember) return reject("memory.verification.unavailable");
    const auto failure_capture = capture_failure_context(false);
    const auto request = engine_->prepare_remember();
    if (!request) return reject("memory.verification.busy");
    const auto& merging = request->merging_record_ids();
    if (merging.size() > 1 && !network_labels_writable_) {
        engine_->cancel(); return reject("memory.verification.writeFailed");
    }
    if (!merging.empty()) {
        auto labels = network_labels_;
        const auto merged = labels.preserve_merged_metadata(merging.front(), merging);
        if (merged == NetworkLabels::MergeResult::conflict || merged == NetworkLabels::MergeResult::unavailable) {
            engine_->cancel();
            return reject(merged == NetworkLabels::MergeResult::conflict ? "memory.verification.nameConflict" : "memory.verification.writeFailed");
        }
    }
    auto preferences = wifi_preferences();
    if (std::any_of(merging.begin(), merging.end(), [&](const auto& id) { return preferences.excluded.count(id) != 0; })) {
        auto next = preferences;
        next.excluded.insert(merging.begin(), merging.end());
        if (next.excluded.size() > 64) {
            preferences.auto_join = false;
            save_wifi_preferences(preferences, true, true);
            engine_->cancel();
            return reject("wifiRecovery.revokeSaveFailed");
        }
        // Persist every possible survivor's opt-out before merging any memory records.
        if (!save_wifi_preferences(next, true, true)) {
            engine_->cancel();
            return reject("wifiRecovery.revokeSaveFailed");
        }
    }
    const auto feedback_generation = ++memory_verification_generation_;
    const auto feedback_epoch = engine_->snapshot().epoch;
    campus_service_only_verification_ = false;
    set_memory_verification_message("memory.verification.checking");
    memory_verification_merge_ids_ = merging;
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false;
    next_due_.reset();
    auto_timer_retry_deadline_.reset();
    jobs_.push_back([this, request = *request, feedback_generation, feedback_epoch, failure_capture] {
        const auto result = engine_->trust_and_remember(request);
        if (result == ConnectionEngine::RememberResult::needsProof) record_operation_failure(failure_capture, "verificationFailed");
        else if (result == ConnectionEngine::RememberResult::failed) record_operation_failure(failure_capture, "memoryWriteFailed");
        else if (result == ConnectionEngine::RememberResult::unreliable) record_operation_failure(failure_capture, "identificationUnavailable");
        {
            std::lock_guard<std::mutex> inbox_lock(inbox_mutex_);
            inbox_remember_result_ = RememberFeedback{result, feedback_generation, feedback_epoch};
        }
        post_changed();
    });
    worker_cv_.notify_all();
    return true;
}

bool AppController::verify_campus_service() {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    const auto reject = [this](const char* key) {
        set_memory_verification_message(key); post_changed(); return false;
    };
    if (stopping_ || shutting_down_) return reject("campus.verify.cancelled");
    if (read_only_) return reject("memory.verification.readOnly");
    if (network_refresh_pending_) return reject("memory.verification.pending");
    const auto state = engine_->snapshot();
    if (engine_busy_ || manual_connection_pending_ || !jobs_.empty() || state.busy) return reject("memory.verification.busy");
    if (!state.can_verify_campus) return reject("campus.verify.disconnected");
    const auto failure_capture = capture_failure_context(false);
    const auto request = engine_->prepare_verify_campus();
    if (!request) return reject("memory.verification.busy");
    const auto generation = ++memory_verification_generation_;
    const auto epoch = state.epoch;
    campus_service_only_verification_ = true;
    set_memory_verification_message("campus.verify.checking");
    campus_verification_schedule_ = CampusVerificationSchedule{auto_armed_ || auto_timer_retry_deadline_.has_value(),
        auto_armed_ ? next_due_ : auto_timer_retry_deadline_, settings_.mode};
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false; next_due_.reset();
    auto_timer_retry_deadline_.reset();
    jobs_.push_back([this, request = *request, generation, epoch, failure_capture] {
        const auto result = engine_->verify_campus(request);
        if (result == ConnectionEngine::CampusVerificationResult::needsProof) record_operation_failure(failure_capture, "verificationFailed");
        else if (result == ConnectionEngine::CampusVerificationResult::unavailable) record_operation_failure(failure_capture, "identificationUnavailable");
        { std::lock_guard<std::mutex> inbox_lock(inbox_mutex_);
          inbox_campus_result_ = CampusFeedback{result, generation, epoch, engine_->snapshot().discovery_revision}; }
        post_changed();
    });
    worker_cv_.notify_all();
    return true;
}

void AppController::restore_campus_verification_schedule() {
    if (!campus_verification_schedule_ || engine_busy_ || engine_->snapshot().busy) return;
    const auto previous = *campus_verification_schedule_;
    campus_verification_schedule_.reset();
    if (!previous.running || previous.mode != settings_.mode || settings_.mode == MonitoringMode::manual || engine_->snapshot().sleeping)
        return;
    const int remaining = previous.deadline ? static_cast<int>(std::ceil(std::max(0.0, *previous.deadline - wall_now()))) : 0;
    const int delay = std::max({30, remaining, engine_->next_check_delay()});
    if (!auto_armed_ || next_due_.value_or(0) < wall_now() + delay) schedule_auto(delay);
}

void AppController::forget_network() {
    if (read_only_) return;
    if (engine_->snapshot().verifying_memory) invalidate_memory_verification("memory.verification.cancelled");
    else clear_memory_verification_message();
    const auto ids = engine_->current_remembered_ids();
    engine_->forget_current_network();
    auto next = wifi_preferences();
    for (const auto& id : ids) { next.manual_names.erase(id); next.excluded.insert(id); }
    if (!ids.empty()) save_wifi_preferences(next, true);
    refresh_network_labels();
    if (engine_->snapshot().memory_write_failed) notice_ = "memory.writeFailed";
}

void AppController::forget_network(const std::string& id) {
    if (read_only_) return;
    const auto before = engine_->remembered_networks();
    if (std::none_of(before.begin(), before.end(), [&](const auto& record) { return record.id == id; })) return;
    engine_->forget_network(id);
    const auto after = engine_->remembered_networks();
    auto next = wifi_preferences();
    bool removed = false;
    for (const auto& record : before) {
        if (std::any_of(after.begin(), after.end(), [&](const auto& remaining) { return record.id == remaining.id; })) continue;
        next.manual_names.erase(record.id); next.excluded.insert(record.id); removed = true;
    }
    if (removed) save_wifi_preferences(next, true);
    refresh_network_labels();
    if (engine_->snapshot().memory_write_failed) notice_ = "memory.writeFailed";
}

void AppController::forget_all_networks() {
    if (read_only_) return;
    if (engine_->snapshot().verifying_memory) invalidate_memory_verification("memory.verification.cancelled");
    else clear_memory_verification_message();
    engine_->forget_all_networks();
    auto next = wifi_preferences(); next.manual_names.clear(); next.auto_join = false;
    save_wifi_preferences(next, true);
    refresh_network_labels();
    if (engine_->snapshot().memory_write_failed) notice_ = "memory.writeFailed";
}

void AppController::set_draft(const std::string& account, const std::string& password) { engine_->set_draft(Credentials{account, password}); }

ConnectionEngine::SaveResult AppController::save_account() {
    if (read_only_) return ConnectionEngine::SaveResult::failed;
    const auto result = engine_->save_draft();
    if (result == ConnectionEngine::SaveResult::saved || result == ConnectionEngine::SaveResult::unchanged)
        credential_read_failed_ = false;
    if (result == ConnectionEngine::SaveResult::saved && settings_.mode == MonitoringMode::recover && !engine_busy_) schedule_auto(2);
    return result;
}

bool AppController::delete_account() {
    if (read_only_) return false;
    invalidate_manual_connection();
    if (memory_verification_pending()) invalidate_memory_verification("memory.verification.cancelled");
    else clear_memory_verification_message();
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        jobs_.clear();
        manual_queued_ = false;
    }
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false; next_due_.reset();
    auto_timer_retry_deadline_.reset();
    clear_system_connectivity();
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.cancel(); }
    const bool removed = engine_->delete_account();
    if (removed) credential_read_failed_ = false;
    internet_service_.invalidate();
    update_wifi_context();
    if (!removed) notice_ = "account.deleteFailed";
    else if (notice_ == "account.deleteFailed" || notice_ == "account.readFailed") notice_.clear();
    post_changed();
    return removed;
}

void AppController::discard_draft() { engine_->revert_draft(); }

void AppController::set_appearance(Appearance appearance) {
    // Preview themes in an observe-only session without changing the stored preference.
    settings_.appearance = appearance;
    save_settings();
}

bool AppController::set_launch(bool enabled) {
    if (read_only_) return false;
    const bool ok = runtime_->set_launch(enabled);
    launch_enabled_ = runtime_->launch_enabled();
    launch_approved_ = runtime_->launch_approved();
    if (!ok) notice_ = "error.launchUpdate";
    else if (notice_ == "error.launchUpdate") notice_.clear();
    return ok;
}

void AppController::set_tutorial(const std::string& status) {
    if (read_only_) return;
    settings_.tutorial = status;
    save_settings();
}

CompatState AppController::compat() const {
    std::lock_guard<std::mutex> lock(checks_mutex_);
    return compat_;
}

std::optional<AppController::ProxyAssistantContext> AppController::proxy_assistant_context(const EngineSnapshot& state) const {
    if (!state.tunnel || !proxy_physical_link_active_ || state.sleeping) return std::nullopt;
    return ProxyAssistantContext{state.epoch, state.authentication_route ? state.authentication_route->identity : std::string()};
}

void AppController::sync_proxy_assistant_context(const EngineSnapshot& state) {
    const auto context = proxy_assistant_context(state);
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    if (context == proxy_suggestion_context_) return;
    proxy_suggestion_context_ = context;
    ++proxy_suggestion_revision_;
    proxy_suggestion_state_ = {};
    proxy_campus_discovery_revision_ = state.discovery_revision;
    proxy_suggestion_round_.reset();
}

ProxyAssistantSuggestionState AppController::proxy_assistant_suggestion() const {
    if (network_refresh_pending_ || shutting_down_) return {};
    const auto context = proxy_assistant_context(engine_->snapshot());
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    if (!context || context != proxy_suggestion_context_) return {};
    auto state = proxy_suggestion_state_;
    state.suggested = state.connection_failures >= 2 || state.campus_failures >= 2;
    state.physical_unidentified = context->physical_identity.empty();
    return state;
}

void AppController::invalidate_proxy_assistant_round(bool reset_failures) {
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    ++proxy_suggestion_revision_;
    proxy_suggestion_round_.reset();
    if (reset_failures) proxy_suggestion_state_ = {};
}

void AppController::record_proxy_assistant_physical_report(const ConnectivityReport& report) {
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    if (report.online()) proxy_suggestion_state_.connection_failures = 0;
    if (!proxy_suggestion_round_ || proxy_suggestion_round_->worker_finished) return;
    proxy_suggestion_round_->physical = report;
}

void AppController::record_proxy_assistant_system_report(std::uint64_t generation, const std::optional<ConnectivityReport>& report) {
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    if (report && report->online()) proxy_suggestion_state_.connection_failures = 0;
    if (!proxy_suggestion_round_ || proxy_suggestion_round_->system_generation != generation) return;
    proxy_suggestion_round_->system = report;
    proxy_suggestion_round_->system_finished = true;
    evaluate_proxy_assistant_round_locked();
}

void AppController::finish_proxy_assistant_round() {
    const auto state = engine_->snapshot();
    if (state.status == EngineStatus::cancelled || state.sleeping || shutting_down_) {
        invalidate_proxy_assistant_round(true);
        return;
    }
    sync_proxy_assistant_context(state);
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    if (proxy_campus_discovery_revision_ != state.discovery_revision) {
        if (state.required_campus_confirmed) {
            proxy_suggestion_state_.campus_failures = 0;
            ++proxy_campus_check_revision_;
        }
        proxy_campus_discovery_revision_ = state.discovery_revision;
    }
    if (!proxy_suggestion_round_) return;
    proxy_suggestion_round_->worker_finished = true;
    evaluate_proxy_assistant_round_locked();
}

void AppController::evaluate_proxy_assistant_round_locked() {
    if (!proxy_suggestion_round_ || !proxy_suggestion_round_->worker_finished || !proxy_suggestion_round_->system_finished) return;
    const auto round = std::move(*proxy_suggestion_round_);
    proxy_suggestion_round_.reset();
    if (!proxy_suggestion_context_ || round.revision != proxy_suggestion_revision_) return;
    const auto complete = [](const ConnectivityReport& report) {
        if (report.complete_result == CompleteCheckResult::cancelled) return false;
        if (report.apple.state == EndpointState::cancelled || report.microsoft.state == EndpointState::cancelled) return false;
        return report.online() || (report.apple.state != EndpointState::notCompleted && report.microsoft.state != EndpointState::notCompleted);
    };
    const bool unidentified = proxy_suggestion_context_->physical_identity.empty();
    // Missing physical authority blocks authentication, not an offline help link.
    // Its unmeasured column is not recorded as a probe failure: only the completed
    // current-system sample and completed controller round contribute here.
    if (!round.physical && !unidentified) return;
    if (!round.system || !complete(*round.system) || round.system->online() ||
        (round.physical && (!complete(*round.physical) || round.physical->online()))) {
        proxy_suggestion_state_.connection_failures = 0;
        return;
    }
    proxy_suggestion_state_.connection_failures = std::min(2u, proxy_suggestion_state_.connection_failures + 1);
}

void AppController::record_proxy_assistant_campus_check(std::uint64_t revision, std::uint64_t campus_revision, const compat::Input& input) {
    std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
    if (!proxy_suggestion_context_ || revision != proxy_suggestion_revision_ || campus_revision != proxy_campus_check_revision_) return;
    for (const auto* value : {&input.campus_direct, &input.campus_system, &input.public_direct, &input.public_system})
        if (*value == "cancelled") { proxy_suggestion_state_.campus_failures = 0; return; }
    if (input.campus_direct == "reachable") { proxy_suggestion_state_.campus_failures = 0; return; }
    for (const auto value : {"dns", "timeout", "tls", "network", "redirect", "authentication", "httpStatus"}) {
        if (input.campus_direct != value) continue;
        proxy_suggestion_state_.campus_failures = std::min(2u, proxy_suggestion_state_.campus_failures + 1);
        return;
    }
}

void AppController::run_compat(bool include_current_path) {
    std::uint64_t generation;
    sync_proxy_assistant_context(engine_->snapshot());
    std::uint64_t suggestion_revision, campus_revision;
    { std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_); suggestion_revision = proxy_suggestion_revision_; campus_revision = proxy_campus_check_revision_; }
    {
        std::lock_guard<std::mutex> lock(checks_mutex_);
        if (compat_.running) return;
        generation = ++compat_generation_;
        compat_ = CompatState{};
        compat_.running = true;
        compat_.current_path_included = include_current_path;
    }
    if (compat_thread_.joinable()) compat_thread_.join();
    compat_token_ = CancelToken();
    const bool proxy_on = proxy_.state == "on";
    const std::string tunnel = network_.tunnel_known ? (network_.tunnel.tunnel ? "detected" : "notDetected") : "unknown";
    CancelToken token = compat_token_;
    compat_thread_ = std::thread([this, proxy_on, tunnel, token, generation, include_current_path, suggestion_revision, campus_revision] {
        compat::Input input = include_current_path ? runtime_->proxy_config_check(proxy_on, tunnel, token) : runtime_->compatibility(proxy_on, tunnel, token);
        compat::Verdict verdict = compat::diagnose(input);
        {
            std::lock_guard<std::mutex> lock(checks_mutex_);
            if (generation != compat_generation_ || token.cancelled()) return;
            compat_.input = input;
            compat_.verdict = verdict;
            compat_.running = false;
            compat_.done = true;
            if (include_current_path) record_proxy_assistant_campus_check(suggestion_revision, campus_revision, input);
        }
        post_changed();
    });
    post_changed();
}

void AppController::cancel_compat() {
    bool was_running;
    { std::lock_guard<std::mutex> lock(checks_mutex_); was_running = compat_.running; ++compat_generation_; compat_ = CompatState{}; }
    compat_token_.cancel();
    if (was_running) {
        std::lock_guard<std::mutex> lock(proxy_suggestion_mutex_);
        ++proxy_suggestion_revision_;
        proxy_suggestion_round_.reset();
        proxy_suggestion_state_.campus_failures = 0;
    }
}

bool AppController::is_failure_event(const std::string& text) const {
    // An unknown submission can be wrapped by a historical failure-event prefix.
    // Its outcome remains unknown, even when the wording of that prefix changes.
    for (const std::string& uncertain : {std::string(copy::text("portalError.authenticationUncertain")),
                                       std::string("登录请求已发出，但暂未收到结果。请先做一次完整检测。"),
                                       std::string("登录已经提交，结果还没确认。先做一次完整检测。")})
        if (!uncertain.empty() && text.find(uncertain) != std::string::npos) return false;
    for (const auto& marker : failure_markers_)
        if (!marker.empty() && text.find(marker) != std::string::npos) return true;
    return false;
}

records::ExportContext AppController::export_context() const {
    records::ExportContext context;
    context.version = kVersion;
    context.build = kBuild;
    context.platform = "windows";
    context.mode = std::string(to_string(settings_.mode));
    context.area = std::string(to_string(settings_.area));
    context.probe_mode = std::string(to_string(settings_.probe));
    context.now = wall_now();
    const auto state = snapshot();
    const bool current_environment = diagnostic_network_fresh_ && !network_refresh_pending_ && !state.sleeping && !shutting_down_;
    records::DiagnosticEnvironment environment;
    environment.refreshing = !current_environment;
    if (current_environment) {
        environment.connection = network_.access;
        environment.system_proxy = proxy_.state;
        environment.tun = network_.tunnel_known ? (network_.tunnel.tunnel ? "detected" : "notDetected") : "unknown";
    }
    context.environment = std::move(environment);
    records::DiagnosticRecovery recovery;
    recovery.state = std::string(to_string(state.auto_login));
    if (!current_environment && state.auto_login == AutoLoginState::ready) recovery.state = "awaitingProof";
    recovery.read_only = state.read_only;
    recovery.paused = state.paused;
    recovery.network_available = current_environment && state.network_available;
    recovery.network_remembered = current_environment && state.remembered;
    recovery.session_trusted = current_environment && state.trusted;
    recovery.cooldown_remaining = state.cooldown_remaining;
    for (const auto& hold : state.holds)
        if ((state.area == CampusArea::automatic && (hold.applies(CampusArea::teaching, state.generation, state.epoch) ||
                hold.applies(CampusArea::dormitory, state.generation, state.epoch))) || hold.applies(state.area, state.generation, state.epoch))
            recovery.hold_reasons.push_back(hold.reason);
    context.recovery = std::move(recovery);
    const auto campus = current_environment ? status::campus_verification_key(state) : "env.campusVerification.notChecked";
    context.campus_state = campus == "env.campusVerification.checking" ? "checking" :
        campus == "env.campusVerification.confirmed" ? "confirmed" :
        campus == "env.campusVerification.unconfirmed" ? "unconfirmed" : "notChecked";
    if (context.campus_state == "unconfirmed" && state.last_discovery)
        context.campus_issue = state.last_discovery->issue;
    if (current_environment && network_.path.requires_physical_binding) {
        context.physical_network = physical::anonymous_diagnostics(network_.physical_diagnostics);
        context.physical_identified = state.authentication_route.has_value();
    }
    return context;
}

namespace {
std::optional<std::wstring> pick_diagnostics_file(HWND owner) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return std::nullopt;
    const COMDLG_FILTERSPEC types[] = {{L"JSON", L"*.json"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"json");
    const std::time_t t = static_cast<std::time_t>(AppController::wall_now());
    std::tm local{};
    localtime_s(&local, &t);
    wchar_t name[64];
    wcsftime(name, 64, L"SZUConnect-diagnostics-%Y%m%d-%H%M%S.json", &local);
    dialog->SetFileName(name);
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::wstring selected(path);
    CoTaskMemFree(path);
    return selected;
}
std::string save_diagnostics_file(const std::wstring& path, const std::string& data, const CancelToken& token) {
    if (token.cancelled()) return "records.exportCancelled";
    if (path.empty() || data.size() > 32 * 1024 * 1024) return "records.exportFailed";
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    static std::atomic<unsigned> sequence{0};
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = path + L".SZUConnect-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(++sequence) + L".tmp";
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) return "records.exportFailed";
    bool ok = true;
    for (std::size_t offset = 0; offset < data.size() && ok && !token.cancelled();) {
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(64 * 1024, data.size() - offset));
        DWORD written = 0;
        ok = WriteFile(file, data.data() + offset, size, &written, nullptr) && written == size;
        offset += written;
    }
    if (ok && !token.cancelled()) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    const bool cancelled = token.cancelled();
    if (ok && !cancelled && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return "records.exportDone";  // cancellation after the commit must not claim the file was discarded
    DeleteFileW(temporary.c_str());
    return cancelled ? "records.exportCancelled" : "records.exportFailed";
}
}

bool AppController::export_diagnostics(HWND owner) {
    if (offline_smoke() || shutting_down_ || diagnostics_export_running_) return false;
    const auto path = diagnostics_save_picker_ ? diagnostics_save_picker_(owner) : pick_diagnostics_file(owner);
    if (!path || path->empty() || shutting_down_) return false;
    auto context = export_context();
    auto health = health_;
    auto events = events_;
    if (diagnostics_export_worker_.joinable()) diagnostics_export_worker_.join();
    diagnostics_export_token_ = CancelToken{};
    const auto token = diagnostics_export_token_;
    diagnostics_export_running_ = true;
    diagnostics_export_feedback_ = "records.exporting";
    try {
        diagnostics_export_worker_ = std::thread([this, path = *path, context = std::move(context),
            health = std::move(health), events = std::move(events), token]() mutable {
            std::string result = "records.exportFailed";
            try {
                context.crashes = crash::anonymous_report(!read_only_ && runtime_->mode() == RuntimeMode::normal ?
                    runtime_->crash_diagnostics(token) : crash::Report{});
                if (token.cancelled()) result = "records.exportCancelled";
                else result = save_diagnostics_file(path, json::serialize(records::diagnostics_export(context, health, events), true), token);
            } catch (...) { result = token.cancelled() ? "records.exportCancelled" : "records.exportFailed"; }
            { std::lock_guard<std::mutex> lock(diagnostics_export_mutex_); diagnostics_export_inbox_ = std::move(result); }
            post_changed();
        });
    } catch (...) {
        diagnostics_export_running_ = false; diagnostics_export_feedback_ = "records.exportFailed";
        post_changed(); return false;
    }
    post_changed();
    return true;
}

void AppController::cancel_diagnostics_export() {
    if (!diagnostics_export_running_) return;
    diagnostics_export_token_.cancel();
    diagnostics_export_feedback_ = "records.exportCancelling";
    post_changed();
}

void AppController::drain_diagnostics_export() {
    std::optional<std::string> result;
    { std::lock_guard<std::mutex> lock(diagnostics_export_mutex_); result.swap(diagnostics_export_inbox_); }
    if (!result) return;
    diagnostics_export_running_ = false;
    diagnostics_export_feedback_ = std::move(*result);
}

bool AppController::copy_diagnostics(HWND owner) {
    if (offline_smoke()) return false;
    return win::copy_to_clipboard(owner, diagnostic_summary_text());
}

std::string AppController::diagnostic_summary_text() const {
    return records::diagnostics_summary_text(export_context(), health_);
}

std::string AppController::diagnostic_export_text() const {
    return json::serialize(records::diagnostics_export(export_context(), health_, events_), true);
}

bool AppController::copy_rules(HWND owner) { return !offline_smoke() && win::copy_to_clipboard(owner, compat::rules_text()); }

void AppController::open_proxy_assistant() {
    cancel_compat();
    close_proxy_assistant();
    proxy_config_has_backup_ = !read_only_ && runtime_->proxy_config_has_backup();
}
void AppController::close_proxy_assistant() {
    cancel_compat();
    runtime_->clear_proxy_config_preview(); proxy_config_preview_ = {}; proxy_config_feedback_.clear();
}
void AppController::set_proxy_config_format(proxyassist::Format format) {
    if (format != proxyassist::Format::clashVergeScript && format != proxyassist::Format::mihomoYaml && format != proxyassist::Format::singBoxJson) return;
    if (format == proxy_config_format_) return;
    cancel_compat();
    close_proxy_assistant(); proxy_config_format_ = format;
}
namespace {
std::string proxy_error_key(const std::string& error) {
    const auto key = "proxyAssistant.error." + error;
    return copy::contains(key) ? key : "proxyAssistant.error.unavailable";
}
}
void AppController::prepare_proxy_config(HWND owner) {
    if (read_only_ || snapshot().busy) return;
    runtime_->clear_proxy_config_preview(); proxy_config_preview_ = {}; proxy_config_feedback_.clear();
    proxy_config_preview_ = runtime_->prepare_proxy_config(owner, proxy_config_format_);
    if (!proxy_config_preview_.error.empty()) {
        proxy_config_feedback_ = proxy_error_key(proxy_config_preview_.error);
        proxy_config_preview_ = {};
        runtime_->clear_proxy_config_preview();
    }
    else if (proxy_config_preview_.ready && !proxy_config_preview_.changed) proxy_config_feedback_ = "proxyAssistant.unchanged";
}
void AppController::apply_proxy_config() {
    if (read_only_ || snapshot().busy || !proxy_config_preview_.error.empty() || !proxy_config_preview_.ready || !proxy_config_preview_.changed) return;
    cancel_compat();
    const auto result = runtime_->apply_proxy_config();
    proxy_config_feedback_ = result.success ? (result.changed ? "proxyAssistant.saved" : "proxyAssistant.unchanged") : proxy_error_key(result.error);
    runtime_->clear_proxy_config_preview(); proxy_config_preview_ = {};
    proxy_config_has_backup_ = runtime_->proxy_config_has_backup();
}
void AppController::restore_proxy_config() {
    if (read_only_ || snapshot().busy) return;
    cancel_compat();
    const auto result = runtime_->restore_proxy_config();
    proxy_config_feedback_ = result.success ? "proxyAssistant.restored" : proxy_error_key(result.error);
    runtime_->clear_proxy_config_preview(); proxy_config_preview_ = {};
    proxy_config_has_backup_ = runtime_->proxy_config_has_backup();
}
bool AppController::copy_proxy_script(HWND owner) {
    return !offline_smoke() && win::copy_to_clipboard(owner, proxyassist::clash_verge_script());
}
void AppController::discard_proxy_config_backup() {
    if (read_only_ || snapshot().busy || !proxy_config_has_backup_) return;
    cancel_compat();
    const auto result = runtime_->discard_proxy_config_backup();
    proxy_config_feedback_ = result.success ? "proxyAssistant.backupDiscarded" : proxy_error_key(result.error);
    runtime_->clear_proxy_config_preview(); proxy_config_preview_ = {};
    proxy_config_has_backup_ = runtime_->proxy_config_has_backup();
}

// ---------------- persistence ----------------
void AppController::load() {
    std::string text;
    const auto settings_status = runtime_->read(L"settings.json", text);
    if (settings_status != win::Storage::ReadStatus::missing) {
        const auto decoded = settings_status == win::Storage::ReadStatus::ok ? decode_settings(text) : std::nullopt;
        if (decoded) settings_ = *decoded;
        else {
            // Leave unknown, damaged and temporarily unreadable originals in place.
            settings_writable_ = false;
            settings_ = Settings{};
            notice_ = "error.settingsUnreadable";
        }
    }
    text.clear();
    const auto websites_status = runtime_->read(L"websites.json", text);
    const auto preferences_status = websites_status == win::Storage::ReadStatus::ok ? WebsitePreferencesState::ReadStatus::found :
        websites_status == win::Storage::ReadStatus::missing ? WebsitePreferencesState::ReadStatus::missing : WebsitePreferencesState::ReadStatus::failed;
    if (!website_preferences_.load(preferences_status, text)) {
        // Do not move, overwrite, or export an unreadable configuration at startup.
        website_configuration_error_ = "internet.settingsUnreadable";
    }
    const auto& websites = website_preferences_.current();
    internet_service_.configure(websites.targets, websites.follow_system_proxy);  // preserves restored quick/complete preference
    text.clear();
    switch (runtime_->read(L"events.json", text)) {
        case win::Storage::ReadStatus::ok:
            if (auto log = records::EventLog::from_json(text)) {
                events_ = std::move(*log);
            } else {
                events_writable_ = !read_only_ && runtime_->quarantine(L"events.json");
                notice_ = events_writable_ ? "error.eventsQuarantined" : "error.eventsUnreadable";
            }
            break;
        case win::Storage::ReadStatus::failed: events_writable_ = false; notice_ = "error.eventsUnreadable"; break;
        case win::Storage::ReadStatus::missing: break;
    }
    text.clear();
    switch (runtime_->read(L"health.json", text)) {
        case win::Storage::ReadStatus::ok:
            if (auto log = records::HealthLog::from_json(text)) {
                health_ = std::move(*log);
                health_.prune(wall_now());
            } else {
                health_writable_ = !read_only_ && runtime_->quarantine(L"health.json");
                notice_ = "error.healthUnreadable";
            }
            break;
        case win::Storage::ReadStatus::failed: health_writable_ = false; notice_ = "error.healthUnreadable"; break;
        case win::Storage::ReadStatus::missing: break;
    }
    if (!runtime_->storage_available()) notice_ = "error.storage";
}

void AppController::save_settings() {
    if (read_only_) return;
    if (!settings_writable_) { notice_ = "error.settingsUnreadable"; return; }
    std::string existing;
    const auto status = runtime_->read(L"settings.json", existing);
    if (status == win::Storage::ReadStatus::failed || (status == win::Storage::ReadStatus::ok && !decode_settings(existing))) {
        settings_writable_ = false;
        settings_.mode = MonitoringMode::manual;
        engine_->set_mode(MonitoringMode::manual);
        update_wifi_context();
        schedule_auto(0);
        notice_ = "error.settingsUnreadable";
        post_changed();
        return;
    }
    json::Value root = json::Value::object();
    root.set("formatVersion", json::Value::number(1));
    root.set("monitoringMode", json::Value::string(std::string(to_string(settings_.mode))));
    root.set("area", json::Value::string(std::string(to_string(settings_.area))));
    root.set("probeMode", json::Value::string(std::string(to_string(settings_.probe))));
    root.set("appearanceMode", json::Value::string(std::string(appearance_name(settings_.appearance))));
    json::Value tutorial = json::Value::object();
    tutorial.set("status", json::Value::string(settings_.tutorial));
    root.set("tutorial", std::move(tutorial));
    if (!runtime_->write(L"settings.json", json::serialize(root, true))) notice_ = "error.writeFailed";
}

void AppController::persist_logs() {
    logs_dirty_ = false;
    if (read_only_) return;
    health_.prune(wall_now());
    bool ok = true;
    if (events_writable_) ok = runtime_->write(L"events.json", json::serialize(events_.to_json())) && ok;
    if (health_writable_) ok = runtime_->write(L"health.json", json::serialize(health_.to_json())) && ok;
    if (!ok) notice_ = "error.writeFailed";
}

}  // namespace szu::app
