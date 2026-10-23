#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "campus_verification_runtime_tests.hpp"

namespace designparityruntime {
class Fake : public campusverificationruntime::Fake {
public:
    explicit Fake(bool routable = false, const std::string& mode = "recover")
        : campusverificationruntime::Fake(mode), routable_(routable) {
        diagnostics.interface_count = 1;
        diagnostics.reject(szu::physical::CandidateRejection::missingDNS);
    }
    szu::win::NetworkSnapshot capture_network() override {
        ++network_captures;
        auto result = campusverificationruntime::Fake::capture_network();
        result.physical_diagnostics = diagnostics;
        result.adapter_names = {"Fixture adapter name"};
        result.current_label = szu::NetworkLabel{"Fixture current Ethernet label", "ethernet", std::nullopt, false};
        if (!routable_) {
            result.path.physical_candidates.clear();
            result.path.descriptors.clear();
        }
        return result;
    }
    szu::physical::CandidateDiagnostics capture_physical_diagnostics() override {
        ++diagnostic_captures;
        if (diagnostic_failure) throw std::runtime_error("fixture anonymous capture failure");
        return diagnostics;
    }
    bool write(const wchar_t* name, const std::string& data) override {
        ++writes;
        return wifiruntime::Fake::write(name, data);
    }
    szu::physical::CandidateDiagnostics diagnostics;
    std::atomic<int> network_captures{0}, diagnostic_captures{0}, writes{0};
    bool diagnostic_failure = false;
private:
    bool routable_;
};

class SystemGate final : public Fake {
public:
    SystemGate() : Fake(true, "manual") {}
    std::optional<szu::ConnectivityReport> system_connectivity(szu::ProbeMode mode, const szu::CancelToken& token) override {
        const int call = ++system_calls;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [&] { return released_ >= call; });
        }
        if (token.cancelled()) ++cancelled;
        ++returned;
        if (!with_report) return std::nullopt;
        auto report = szu::ConnectivityReport::simple(true, true);
        report.mode = mode;
        return report;
    }
    void release(int count) {
        { std::lock_guard<std::mutex> lock(mutex_); released_ = count; }
        ready_.notify_all();
    }
    std::atomic<int> system_calls{0}, returned{0}, cancelled{0};
    std::atomic<bool> with_report{true};
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    int released_ = 0;
};

inline auto holds(const szu::EngineSnapshot& state) {
    using Item = std::tuple<szu::HoldReason, std::optional<szu::CampusArea>, std::optional<std::uint64_t>,
        std::optional<std::uint64_t>, szu::HoldOrigin, std::string>;
    std::vector<Item> result;
    for (const auto& hold : state.holds)
        result.emplace_back(hold.reason, hold.area, hold.generation, hold.epoch, hold.origin, hold.rejection);
    return result;
}
}

template<class Check> void run_design_parity_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const auto mode : {RuntimeMode::normal, RuntimeMode::observeOnly}) {
        auto services = std::make_unique<designparityruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, mode, std::move(services)); app.start();
        if (mode == RuntimeMode::normal) app.pause();
        const auto before = app.snapshot();
        const auto due = app.next_check();
        const auto files = fake->files;
        const int captures = fake->network_captures, writes = fake->writes, credential_reads = fake->credential_reads;
        check(app.physical_identification_needed() && !before.authentication_route &&
            app.physical_identification_detail() == "env.identification.reason.missingDNS" && app.identification_feedback().empty(),
            "unbound physical Ethernet has an actionable anonymous reason before a manual diagnostic refresh");
        app.refresh_physical_identification();
        check(fake->diagnostic_captures == 1 && fake->network_captures == captures &&
            app.identification_feedback() == "env.identification.refreshed" &&
            app.network().physical_diagnostics.rejections.count(physical::CandidateRejection::missingDNS),
            "local diagnostic refresh obtains anonymous counters without recapturing or applying a network path");
        fake->diagnostics.rejections.clear(); fake->diagnostics.candidate_count = 1;
        app.refresh_physical_identification();
        const auto after = app.snapshot();
        check(app.physical_identification_needed() && app.physical_identification_detail() == "env.identification.awaitingRefresh" &&
            app.network().physical_diagnostics.candidate_count == 1 && !after.authentication_route && !after.can_remember && !after.can_verify_campus,
            "newly valid diagnostic candidates remain observations and cannot grant a route, memory or campus-verification admission");
        check(after.epoch == before.epoch && after.generation == before.generation && after.status == before.status &&
            after.trusted == before.trusted && after.remembered == before.remembered && after.memory_count == before.memory_count &&
            after.paused == before.paused && designparityruntime::holds(after) == designparityruntime::holds(before) &&
            app.next_check() == due && fake->files == files && fake->writes == writes,
            "anonymous refresh preserves network epoch, memory, trust, protection holds, stored files and the recovery deadline");
        fake->diagnostic_failure = true; app.refresh_physical_identification();
        check(app.identification_feedback() == "env.identification.readFailed" &&
            app.network().physical_diagnostics.candidate_count == 1 && app.snapshot().epoch == before.epoch &&
            fake->network_captures == captures && fake->credential_reads == credential_reads && fake->probes == 0 &&
            fake->system_probes == 0 && fake->discoveries == 0 && fake->writes == writes &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "failed anonymous refresh retains prior observations and never probes, logs in, finds Wi-Fi, reads credentials or writes data");
        const auto summary = app.diagnostic_summary_text();
        check(summary.find("Fixture adapter name") == std::string::npos && summary.find("192.0.2.") == std::string::npos &&
            summary.find("Fixture laboratory cable") == std::string::npos,
            "identification diagnostics omit adapter names, source addresses and saved local remarks from copied text");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0,
            "identification tests use only injected services and in-memory data in ordinary and observe-only sessions");
    }
    {
        auto services = std::make_unique<designparityruntime::SystemGate>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.system_checked_at() && !app.system_checked_mode(), "system column has no invented completion time or mode before its first report");
        app.check();
        wifiruntime::wait_for([&] { app.on_changed(); return fake->system_calls == 1 && fake->probes == 1 && !app.snapshot().busy; });
        check(app.system_connectivity_running() && !app.system_connectivity() && !app.system_checked_at() && !app.system_checked_mode(),
            "physical completion cannot stamp a still-running system connectivity column");
        fake->release(1);
        wifiruntime::wait_for([&] { app.on_changed(); return app.system_connectivity().has_value() && !app.system_connectivity_running(); });
        const auto checked = app.system_checked_at(), physical_checked = app.last_checked();
        check(checked && app.system_checked_mode() == ProbeMode::quick && app.internet_report() &&
            app.internet_report()->base.mode == ProbeMode::quick,
            "each completed column records its actual report scope rather than borrowing the selected or physical mode");
        app.set_probe(ProbeMode::complete);
        check(app.system_checked_at() == checked && app.system_checked_mode() == ProbeMode::quick &&
            app.last_checked() == physical_checked && fake->system_calls == 1 && fake->probes == 1,
            "scope selection preserves both old report times and modes and starts no network request");
        app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 2; });
        wifiruntime::wait_for([&] { app.on_changed(); return fake->probes == 2 && !app.snapshot().busy; });
        app.set_probe(ProbeMode::quick);
        fake->release(2);
        wifiruntime::wait_for([&] { return app.system_connectivity().has_value() && !app.system_connectivity_running(); });
        check(app.system_checked_mode() == ProbeMode::complete && app.internet_report() &&
            app.internet_report()->base.mode == ProbeMode::complete && fake->system_calls == 2 && fake->probes == 2 &&
            app.settings().probe == ProbeMode::quick,
            "both columns use the round's captured complete scope even if the next selection changes before system completion");
        app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 3; });
        app.cancel(); fake->release(3);
        wifiruntime::wait_for([&] { return fake->returned == 3; });
        check(!app.system_checked_at() && !app.system_checked_mode() && !app.system_connectivity() && fake->cancelled == 1,
            "a successful system response returned after cancellation cannot republish an old result, timestamp or mode");
        fake->with_report = false; app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 4; });
        fake->release(4);
        wifiruntime::wait_for([&] { return fake->returned == 4 && !app.system_connectivity_running(); });
        check(!app.system_checked_at() && !app.system_checked_mode() && !app.system_connectivity(),
            "a completed system request without a report still has no displayed completion time or scope");
        fake->release(100); app.shutdown();
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
            fake->evidence().network_calls == 0, "system-column regression uses fake public probes without campus authentication or native network access");
    }
    {
        auto services = std::make_unique<designparityruntime::SystemGate>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1; });
        fake->changed = true; app.refresh_environment();
        fake->release(1);
        wifiruntime::wait_for([&] { app.on_changed(); return fake->system_calls >= 2; });
        check(fake->cancelled == 1 && !app.system_checked_at() && !app.system_checked_mode() && !app.system_connectivity(),
            "a late system response from the old cable cannot fill the new connection column while its replacement remains pending");
        app.cancel(); fake->release(100); app.shutdown();
    }
    {
        auto services = std::make_unique<designparityruntime::Fake>(true, "manual"); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.snapshot().remembered && app.authentication_target_name() == "Fixture laboratory cable",
            "the current remembered attachment displays its saved local remark ahead of system or live adapter labels");
        check(app.physical_system_primary_name() == "Fixture Ethernet" &&
            app.physical_connection_names() == std::vector<std::string>{"Fixture Ethernet"},
            "physical connection labels use the selected physical candidates rather than the all-adapter summary");
        const auto files = fake->files;
        const auto summary = app.diagnostic_summary_text();
        check(summary.find("Fixture laboratory cable") == std::string::npos &&
            summary.find("Fixture current Ethernet label") == std::string::npos && summary.find("Fixture adapter name") == std::string::npos &&
            summary.find("192.0.2.") == std::string::npos && fake->files == files,
            "rendering a local attachment remark and producing diagnostics neither exports it nor changes stored data");
        app.shutdown();
    }
}
