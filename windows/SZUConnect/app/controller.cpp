#include "controller.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <ctime>

#include "shared_copy.hpp"
#include "szu/json.hpp"

namespace szu::app {

using Microsoft::WRL::ComPtr;

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

AppController::AppController(HWND window, RuntimeMode mode, std::unique_ptr<RuntimeServices> runtime) : window_(window), read_only_(mode == RuntimeMode::observeOnly),
    runtime_(runtime ? std::move(runtime) : make_runtime(mode)), internet_service_(*runtime_, [this](const std::string& url, bool follow, const CancelToken& token) {
        return runtime_->check_website(url, follow, token);
    }) {
    ConnectionEngine::Hooks hooks;
    hooks.now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    hooks.sleep = [this](double seconds) { sleep_interruptible(seconds); };
    hooks.persist_credentials = [this](const Credentials& c) { return !read_only_ && runtime_->write_credentials(c); };
    hooks.wall_now = [] { return wall_now(); };
    hooks.persist_recovery = [this](const std::string& data) { return runtime_->write(L"recovery.json", data); };
    hooks.persist_memory = [this](const std::string& data) { return runtime_->write(L"network-memory.json", data); };  // atomic; failure keeps the old file
    hooks.random_hex = [this] { return runtime_->random_hex(16); };
    hooks.changed = [this] { post_changed(); };
    hooks.event = [this](const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_events_.push_back(text);
        }
        post_changed();
    };
    hooks.report = [this](const ConnectivityReport& report) {
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            inbox_reports_.push_back(report);
        }
        post_changed();
    };
    if (read_only_) { hooks.persist_credentials = {}; hooks.persist_recovery = {}; hooks.persist_memory = {}; }
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
    for (const char* legacy : {"登录未完成：", "手动检测未通过：", "外网检测暂未通过，等待复测确认。", "拒绝了登录"})
        failure_markers_.emplace_back(legacy);
}

AppController::~AppController() { shutdown(); }

void AppController::start() {
    load();
    Credentials saved;
    std::optional<Credentials> stored;
    switch (runtime_->read_credentials(saved)) {
        case win::CredentialVault::ReadStatus::found: stored = saved; break;
        case win::CredentialVault::ReadStatus::failed: notice_ = "account.readFailed"; break;
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
            const auto report = runtime_->system_connectivity(request.token);
            {
                std::lock_guard<std::mutex> lock(system_probe_mutex_);
                if (!system_probe_stopping_ && request.generation == system_probe_generation_ && !request.token.cancelled()) {
                    system_report_ = report;
                    system_probe_running_ = false;
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
    for (const auto id : {kTimerAuto, kTimerNetwork, kTimerPersist, kTimerWake, kTimerWiFi}) KillTimer(window_, id);
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
    if (logs_dirty_) persist_logs();
}

void AppController::sleep_interruptible(double seconds) {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_cv_.wait_for(lock, std::chrono::milliseconds(static_cast<long long>(seconds * 1000)), [this] { return stopping_; });
}

void AppController::post_changed() {
    if (!shutting_down_ && !posted_.exchange(true)) PostMessageW(window_, kMsgChanged, 0, 0);
}

void AppController::run_engine(std::function<void()> job, bool automatic) {
    // Admission must happen before the serial worker queue. Waiting until the manual job
    // reaches core::run would leave it blocked behind a hanging background website probe.
    // The engine checks operation kind + stage atomically, so a concurrent transition into
    // discovery/submission/verification cannot be cancelled by this manual request.
    if (!automatic) {
        if (engine_->cancel_automatic_probe()) internet_service_.invalidate();
        else if (engine_->snapshot().busy) return; // ignore clicks during protected or existing foreground work
    }
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (stopping_) return;
    if (automatic && (engine_busy_ || !jobs_.empty())) return;  // never queue background rounds behind other work
    if (!automatic && (manual_queued_ || !jobs_.empty())) return;
    if (!automatic) {
        manual_queued_ = true;
        jobs_.push_back([this, job = std::move(job)] {
            struct ClearPending {
                std::atomic<bool>& value;
                ~ClearPending() { value = false; }
            } clear{manual_queued_};
            job();
        });
    } else jobs_.push_back(std::move(job));
    worker_cv_.notify_all();
}

void AppController::schedule_auto(int seconds) {
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false;
    next_due_.reset();
    if (settings_.mode == MonitoringMode::manual) return;
    const int delay = std::max(0, seconds);
    // Win32 timers clamp below USER_TIMER_MINIMUM. A zero-delay fast round must not
    // silently become a one/five-second wait, and a nonzero round keeps its exact policy delay.
    SetTimer(window_, kTimerAuto, delay == 0 ? 1u : static_cast<UINT>(delay) * 1000u, nullptr);
    auto_armed_ = true;
    next_due_ = wall_now() + delay;
}

void AppController::on_changed() {
    posted_ = false;
    std::vector<std::string> events;
    std::vector<ConnectivityReport> reports;
    std::optional<RememberFeedback> remember_result;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        events.swap(inbox_events_);
        reports.swap(inbox_reports_);
        remember_result.swap(inbox_remember_result_);
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
                case ConnectionEngine::RememberResult::busy: key = "memory.verification.busy"; break;
                case ConnectionEngine::RememberResult::cancelled: key = "memory.verification.cancelled"; break;
                case ConnectionEngine::RememberResult::needsProof: break;
            }
            set_memory_verification_message(key);
        }
    }
    refresh_network_labels();
    const double now = wall_now();
    for (auto& text : events) logs_dirty_ |= events_.append(now, text, is_failure_event(text));
    const auto latest_internet = internet_service_.latest();
    for (std::size_t i = 0; i < reports.size(); ++i) {
        int site_total = -1, site_passed = -1;  // aggregate only; addresses never reach the history
        if (i + 1 == reports.size() && latest_internet && latest_internet->base.mode == ProbeMode::complete) {
            site_total = static_cast<int>(latest_internet->selected_count);
            const int base_passed = (latest_internet->base.apple.succeeded() ? 1 : 0) + (latest_internet->base.microsoft.succeeded() ? 1 : 0);
            site_passed = std::max(0, std::min(site_total, static_cast<int>(latest_internet->passed_count()) - base_passed));
        }
        health_.append(records::sample_from(reports[i], now, site_total, site_passed));
        last_checked_ = now;
        logs_dirty_ = true;
    }
    if (logs_dirty_ && !read_only_) SetTimer(window_, kTimerPersist, 1500, nullptr);
    if (!engine_busy_ && !auto_armed_ && settings_.mode != MonitoringMode::manual) schedule_auto(engine_->next_check_delay());
}

void AppController::on_network_signal() {
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
            KillTimer(window_, kTimerAuto);
            auto_armed_ = false;
            next_due_.reset();
            if (engine_busy_) return;  // on_changed re-arms once the current operation ends
            run_engine([this] {
                // The IP/route watchers may not fire for an association change. Capture on the
                // worker, before any probe or credential decision, to keep the UI responsive.
                const auto fresh = runtime_->capture_network();
                const auto ssid = fresh.current_label && fresh.current_label->kind == "wifi"
                    ? fresh.current_label->ssid : std::nullopt;
                const bool association_changed = !fresh.path.requires_physical_binding && last_auto_wifi_ssid_ && ssid && *last_auto_wifi_ssid_ != *ssid;
                const bool path_changed = last_auto_identity_ != fresh.path.identity;
                const auto previous_epoch = engine_->snapshot().epoch;
                if (association_changed) {
                    // SSID is revocation evidence only. It is never a remembered-network key.
                    engine_->receive_path(PathSnapshot{});
                }
                engine_->receive_path(fresh.path);
                if (engine_->snapshot().epoch != previous_epoch) { internet_service_.invalidate(); clear_system_connectivity(true); }
                if (ssid) last_auto_wifi_ssid_ = ssid;
                last_auto_identity_ = fresh.path.identity;
                if (association_changed || path_changed) PostMessageW(window_, kMsgNetwork, 0, 0);
                refresh_system_connectivity();
                engine_->automatic_round();
            }, true);
            break;
        case kTimerNetwork:
            KillTimer(window_, kTimerNetwork);
            network_refresh_timer_armed_ = false;
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
            if (settings_.mode != MonitoringMode::manual && !engine_busy_) schedule_auto(engine_->next_check_delay());
            post_changed();
            break;
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
        case kTimerWiFi:
            update_wifi_context();
            if (!engine_busy_) run_engine([this] { run_wifi_recovery(); }, true);
            break;
        default: break;
    }
}

void AppController::on_suspend() {
    if (!memory_verification_message_.empty()) invalidate_memory_verification("memory.verification.changed");
    clear_system_connectivity();
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.cancel(); }
    internet_service_.invalidate();
    engine_->set_sleeping(true);  // cancels work and revokes network trust
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false;
    next_due_.reset();
    cancel_compat();
}

void AppController::on_resume() {
    engine_->set_sleeping(false);
    SetTimer(window_, kTimerWake, 1000, nullptr);  // fresh physical binding, then policy controls retries
}

void AppController::refresh_network() {
    auto latest = runtime_->capture_network();
    if (latest.path.identity != network_.path.identity || latest.path.usable != network_.path.usable ||
        latest.path.tunnel != network_.path.tunnel) { internet_service_.invalidate_website_path(); cancel_compat(); }
    network_ = std::move(latest);
    const auto previous_epoch = engine_->snapshot().epoch;
    engine_->receive_path(network_.path);
    if (engine_->snapshot().epoch != previous_epoch) {
        internet_service_.invalidate();
        clear_system_connectivity(network_refresh_pending_ && settings_.mode != MonitoringMode::manual && !engine_->snapshot().sleeping);
    }
    sync_memory_verification_epoch();
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

void AppController::refresh_network_labels() {
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

bool AppController::rename_network(const std::string& id, const std::string& name) {
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
bool AppController::save_wifi_preferences(const wifi::Preferences& next, bool revoking) {
    if (read_only_) return false;
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
    if (!is_wifi_record(id)) return false;
    auto next = wifi_preferences();
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
    post_changed();
}

void AppController::check() {
    run_engine([this] { check_with_system_connectivity(); });
}
std::optional<ConnectivityReport> AppController::system_connectivity() const {
    std::lock_guard<std::mutex> lock(system_probe_mutex_); return system_report_;
}
bool AppController::system_connectivity_running() const {
    std::lock_guard<std::mutex> lock(system_probe_mutex_); return system_probe_running_ || system_probe_recheck_pending_;
}
void AppController::clear_system_connectivity(bool recheck_after_network_refresh) {
    CancelToken stale;
    {
        std::lock_guard<std::mutex> lock(system_probe_mutex_);
        // Keep only the intent to replace a previously requested display check, not
        // its result or token. Explicit cancellation, sleep and shutdown revoke it.
        system_probe_recheck_pending_ = recheck_after_network_refresh && !system_probe_stopping_ && !shutting_down_ &&
            (system_probe_recheck_pending_ || system_probe_running_ || system_report_.has_value() || system_probe_pending_.has_value());
        ++system_probe_generation_; system_report_.reset(); stale = system_probe_token_;
        system_probe_running_ = false;
        system_probe_pending_.reset();
    }
    stale.cancel();
}
void AppController::refresh_system_connectivity(bool recheck_only) {
    CancelToken stale;
    {
        std::lock_guard<std::mutex> lock(system_probe_mutex_);
        if (system_probe_stopping_ || shutting_down_ || (recheck_only && !system_probe_recheck_pending_)) return;
        system_probe_recheck_pending_ = false;
        stale = system_probe_token_;
        system_probe_token_ = CancelToken{};
        ++system_probe_generation_;
        system_report_.reset();
        system_probe_running_ = true;
        // A single pending slot coalesces repeated rounds. Every superseded token is
        // cancelled, and only the newest generation can publish display evidence.
        system_probe_pending_ = SystemProbeRequest{system_probe_token_, system_probe_generation_};
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
    run_engine([this] { refresh_system_connectivity(); engine_->connect(); });
}
void AppController::force_login() {
    if (read_only_ || settings_.mode == MonitoringMode::observe) return;
    const auto state = engine_->snapshot();
    if (!state.last_report || !state.last_report->online() ||
        state.last_report->complete_result == CompleteCheckResult::cancelled ||
        state.last_report->has_recovery_evidence() ||
        !(state.offer_forced_login || state.last_report->complete_partial())) return;
    const auto title = win::widen(copy::text("internet.partial.loginWarning.title"));
    const auto body = win::widen(copy::text("internet.partial.loginWarning.body"));
    // All UI force actions pass here. Cancel, close and any error leave the
    // engine untouched; the explicit continuation still uses the original core guards.
    if (MessageBoxW(window_, body.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    run_engine([this] { refresh_system_connectivity(); engine_->connect_even_if_online(); });
}
void AppController::login_once() { connect(); }
void AppController::cancel() {
    if (engine_->snapshot().verifying_memory || memory_verification_message_ == "memory.verification.checking" ||
        memory_verification_message_ == "memory.verification.pending")
        invalidate_memory_verification("memory.verification.cancelled");
    clear_system_connectivity();
    { std::lock_guard<std::mutex> lock(wifi_mutex_); wifi_schedule_.cancel(); }
    engine_->cancel(); internet_service_.invalidate();
}

void AppController::set_mode(MonitoringMode mode) {
    if (read_only_) return;
    if (!settings_writable_ && mode != MonitoringMode::manual) {
        notice_ = "error.settingsUnreadable"; post_changed(); return;
    }
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
    if (settings_.area != area && !memory_verification_message_.empty())
        invalidate_memory_verification("memory.verification.changed");
    settings_.area = area;
    engine_->set_area(area);
    save_settings();
}

void AppController::set_probe(ProbeMode mode) {
    // Observe-only may choose a session-local check; save_settings still forbids persistence.
    std::lock_guard<std::mutex> gate(worker_mutex_);
    if (stopping_ || engine_busy_ || !jobs_.empty() || engine_->snapshot().busy) return;
    if (settings_.probe != mode) { engine_->cancel(); internet_service_.invalidate(); clear_system_connectivity(); }
    settings_.probe = mode;
    engine_->set_probe_mode(mode);
    save_settings();
}

bool AppController::save_website_preferences(const WebsitePreferences& preferences) {
    if (read_only_) return false;
    // The queue lock covers disk commit too: a queued authentication job cannot start in the
    // gap between the busy check and applying configuration. No in-flight operation is cancelled.
    std::lock_guard<std::mutex> gate(worker_mutex_);
    if (stopping_ || engine_busy_ || !jobs_.empty() || engine_->snapshot().busy) {
        website_configuration_error_ = "internet.settingsBusy";
        return false;
    }
    if (!preferences.encode()) { website_configuration_error_ = "website.error.invalid"; return false; }
    const bool saved = website_preferences_.save(preferences, [this](const std::string& data, bool preserve) {
        return runtime_->write_preserving(L"websites.json", data, preserve);
    });
    if (!saved) { website_configuration_error_ = "internet.settingsWriteFailed"; post_changed(); return false; }
    // Runtime and stale-result generation change only after the durable write succeeds.
    const auto& current = website_preferences_.current();
    internet_service_.configure(current.targets, current.follow_system_proxy);
    engine_->reset_partial_confirmation();
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
    if (memory_verification_epoch_ && *memory_verification_epoch_ != engine_->snapshot().epoch)
        return "memory.verification.changed";
    return memory_verification_message_;
}

void AppController::set_memory_verification_message(std::string key) {
    memory_verification_message_ = std::move(key);
    memory_verification_epoch_ = engine_->snapshot().epoch;
}

void AppController::invalidate_memory_verification(const char* key) {
    ++memory_verification_generation_;
    set_memory_verification_message(key);
}

void AppController::sync_memory_verification_epoch() {
    if (memory_verification_epoch_ && *memory_verification_epoch_ != engine_->snapshot().epoch)
        invalidate_memory_verification("memory.verification.changed");
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
    if (engine_busy_ || !jobs_.empty() || state.busy) return reject("memory.verification.busy");
    if (state.memory_unreadable) return reject("memory.verification.unreadable");
    if (!state.network_available || state.sleeping) return reject("memory.verification.unavailable");
    if (state.remember_blocker == "unreliable") return reject("memory.verification.unreliable");
    if (!state.can_remember) return reject("memory.verification.unavailable");
    const auto request = engine_->prepare_remember();
    if (!request) return reject("memory.verification.busy");
    const auto feedback_generation = ++memory_verification_generation_;
    const auto feedback_epoch = engine_->snapshot().epoch;
    set_memory_verification_message("memory.verification.checking");
    KillTimer(window_, kTimerAuto);
    auto_armed_ = false;
    next_due_.reset();
    jobs_.push_back([this, request = *request, feedback_generation, feedback_epoch] {
        const auto result = engine_->trust_and_remember(request);
        {
            std::lock_guard<std::mutex> inbox_lock(inbox_mutex_);
            inbox_remember_result_ = RememberFeedback{result, feedback_generation, feedback_epoch};
        }
        post_changed();
    });
    worker_cv_.notify_all();
    return true;
}

void AppController::forget_network() {
    if (engine_->snapshot().verifying_memory) invalidate_memory_verification("memory.verification.cancelled");
    else { ++memory_verification_generation_; memory_verification_message_.clear(); memory_verification_epoch_.reset(); }
    const auto ids = engine_->current_remembered_ids();
    engine_->forget_current_network();
    auto next = wifi_preferences();
    for (const auto& id : ids) { next.manual_names.erase(id); next.excluded.insert(id); }
    if (!ids.empty()) save_wifi_preferences(next, true);
    refresh_network_labels();
    if (engine_->snapshot().memory_write_failed) notice_ = "memory.writeFailed";
}

void AppController::forget_all_networks() {
    if (engine_->snapshot().verifying_memory) invalidate_memory_verification("memory.verification.cancelled");
    else { ++memory_verification_generation_; memory_verification_message_.clear(); memory_verification_epoch_.reset(); }
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
    if (result == ConnectionEngine::SaveResult::saved && settings_.mode == MonitoringMode::recover && !engine_busy_) schedule_auto(2);
    return result;
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

void AppController::run_compat() {
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(checks_mutex_);
        if (compat_.running) return;
        generation = ++compat_generation_;
        compat_ = CompatState{};
        compat_.running = true;
    }
    if (compat_thread_.joinable()) compat_thread_.join();
    compat_token_ = CancelToken();
    const bool proxy_on = proxy_.state == "on";
    const std::string tunnel = network_.tunnel_known ? (network_.tunnel.tunnel ? "detected" : "notDetected") : "unknown";
    CancelToken token = compat_token_;
    compat_thread_ = std::thread([this, proxy_on, tunnel, token, generation] {
        compat::Input input = runtime_->compatibility(proxy_on, tunnel, token);
        compat::Verdict verdict = compat::diagnose(input);
        {
            std::lock_guard<std::mutex> lock(checks_mutex_);
            if (generation != compat_generation_ || token.cancelled()) return;
            compat_.input = input;
            compat_.verdict = verdict;
            compat_.running = false;
            compat_.done = true;
        }
        post_changed();
    });
    post_changed();
}

void AppController::cancel_compat() {
    { std::lock_guard<std::mutex> lock(checks_mutex_); ++compat_generation_; compat_ = CompatState{}; }
    compat_token_.cancel();
}

bool AppController::is_failure_event(const std::string& text) const {
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
    return context;
}

bool AppController::export_diagnostics(HWND owner) {
    if (offline_smoke()) return false;
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return false;
    const COMDLG_FILTERSPEC types[] = {{L"JSON", L"*.json"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"json");
    const std::time_t t = static_cast<std::time_t>(wall_now());
    std::tm local{};
    localtime_s(&local, &t);
    wchar_t name[64];
    wcsftime(name, 64, L"SZUConnect-diagnostics-%Y%m%d-%H%M%S.json", &local);
    dialog->SetFileName(name);
    if (FAILED(dialog->Show(owner))) return false;  // includes the user cancelling
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    const std::string data = json::serialize(records::diagnostics_export(export_context(), health_, events_), true);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CoTaskMemFree(path);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == data.size();
}

bool AppController::copy_diagnostics(HWND owner) {
    if (offline_smoke()) return false;
    return win::copy_to_clipboard(owner, records::diagnostics_summary_text(export_context(), health_));
}

bool AppController::copy_rules(HWND owner) { return !offline_smoke() && win::copy_to_clipboard(owner, compat::rules_text()); }

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
    bool ok = true;
    if (events_writable_) ok = runtime_->write(L"events.json", json::serialize(events_.to_json())) && ok;
    if (health_writable_) ok = runtime_->write(L"health.json", json::serialize(health_.to_json())) && ok;
    if (!ok) notice_ = "error.writeFailed";
}

}  // namespace szu::app
