#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include "campus_verification_runtime_tests.hpp"
#include "proxy_assistant_runtime_tests.hpp"

namespace networkrecoveryscheduleruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    Fake() : campusverificationruntime::Fake("recover") {}
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        if (throw_capture.exchange(false)) throw std::runtime_error("fixture capture failure");
        auto result = campusverificationruntime::Fake::capture_network();
        result.path.identity = tunnel ? "fixture-virtual-route" : "fixture-system-route";
        result.path.tunnel = tunnel;
        if (!candidate_available) result.path.physical_candidates.clear();
        return result;
    }
    bool set_expected_physical_route(const std::optional<szu::physical::Route>& attachment) override {
        ++bindings;
        return candidate_available && attachment && *attachment == route();
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto result = szu::ConnectivityReport::simple(online, online); result.mode = mode;
        if (!online) result.apple.state = result.microsoft.state = szu::EndpointState::timeout;
        if (session_expired) {
            result.microsoft.state = szu::EndpointState::redirect;
            result.microsoft.campus_area_hint = szu::CampusPortalHint::teaching;
            result.microsoft.authentication_required = true;
        }
        return result;
    }
    szu::CampusDiscovery discover(szu::CampusArea area, const szu::CancelToken& token) override {
        if (throw_discovery.exchange(false)) { ++discoveries; ++returned; throw std::runtime_error("fixture discovery failure"); }
        return campusverificationruntime::Fake::discover(area, token);
    }
    szu::LoginAttempt attempt(const szu::Credentials& credentials, szu::CampusArea area, const szu::CancelToken&) override {
        ++evidence().fake_auth_submissions;
        fake_credentials = credentials.account == "fixture-user" && credentials.password == "fixture-password";
        selected_area = area; online = true; session_expired = false;
        szu::LoginAttempt result; result.outcome = szu::Outcome::success;
        result.credential_submission_started = true; return result;
    }
    szu::win::CredentialVault::ReadStatus read_credentials(szu::Credentials& credentials) override {
        credentials = {"fixture-user", "fixture-password"}; return szu::win::CredentialVault::ReadStatus::found;
    }
    std::atomic<int> captures{0}, bindings{0};
    std::atomic<bool> tunnel{false}, candidate_available{true}, online{true}, fake_credentials{false};
    std::atomic<bool> session_expired{false}, throw_discovery{false};
    std::atomic<bool> throw_capture{false};
    szu::CampusArea selected_area = szu::CampusArea::automatic;
};
inline void finish(szu::app::AppController& app, const std::function<bool()>& completed) {
    // First let the queued worker enter and finish. Consuming the constructor's
    // pending UI message before the worker begins would arm an unrelated 0s timer.
    wifiruntime::wait_for([&] { return completed() && !app.snapshot().busy; });
    wifiruntime::wait_for([&] { app.on_changed(); return completed() && !app.snapshot().busy && app.next_check(); });
    app.on_changed();
}
}

template<class Check> void run_network_recovery_schedule_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using networkrecoveryscheduleruntime::Fake;
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1 && fake->returned == 1; });
        const int captures = fake->captures;
        fake->throw_capture = true; app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->captures > captures; });
        check(!app.snapshot().busy && app.next_check() && fake->evidence().fake_auth_submissions == 0,
            "a physical snapshot exception cannot leave the controller busy or permanently suppress monitoring");
        fake->throw_discovery = true;
        const auto deadline = app.next_check();
        const int returned = fake->returned;
        check(app.verify_campus_service(), "independent verification remains available after a failed network snapshot");
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->returned > returned; });
        check(app.next_check() && deadline && *app.next_check() >= *deadline && !app.snapshot().verifying_campus &&
            !app.snapshot().busy && fake->evidence().fake_auth_submissions == 0,
            "a verification exception releases its reservation and restores the original monitoring deadline without a submission");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes >= 2; });
        check(app.snapshot().online && app.next_check(), "automatic monitoring continues after verification failure without manual resume");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1 && fake->returned == 1; });
        const auto epoch = app.snapshot().epoch;
        fake->session_expired = true;
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 2; });
        check(app.next_check() && !app.snapshot().busy && fake->evidence().fake_auth_submissions == 0,
            "same-link session expiry with one reachable public probe finishes and renews its confirmation timer");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->evidence().fake_auth_submissions == 1; });
        check(app.snapshot().online && app.snapshot().epoch == epoch && app.next_check() && !app.snapshot().busy &&
            fake->fake_credentials && fake->selected_area == CampusArea::teaching && fake->evidence().fake_wifi_calls == 0,
            "a renewed timer completes one campus-session recovery without reconnecting Wi-Fi or receiving a network notification");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->online = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto epoch = app.snapshot().epoch;
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1; });
        fake->throw_discovery = true;
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->returned == 1; });
        check(!app.snapshot().busy && app.next_check() && fake->evidence().fake_auth_submissions == 0,
            "a discovery exception releases busy state and renews monitoring without a submission");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->evidence().fake_auth_submissions == 1; });
        check(app.snapshot().online && !app.snapshot().busy && app.next_check() && app.snapshot().epoch == epoch,
            "the next timer after a failed task can recover the same link without manual resume");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1 && fake->returned == 1; });
        const auto original = app.next_check();
        const auto epoch = app.snapshot().epoch;
        const auto memory = fake->files[L"network-memory.json"];
        const int captures = fake->captures, probes = fake->probes, discoveries = fake->discoveries;
        check(original && *original > AppController::wall_now() + 10 && *original <= AppController::wall_now() + 15 && app.snapshot().remembered &&
            app.snapshot().required_campus_confirmed, "a remembered physical attachment begins with its real scheduled monitoring round");
        for (int notification = 0; notification < 8; ++notification) {
            fake->tunnel = !fake->tunnel; app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
            check(app.snapshot().epoch == epoch && app.next_check() == original,
                "repeated virtual-route changes cannot postpone or advance the same physical attachment's existing deadline");
        }
        check(fake->captures >= captures + 8 && fake->probes == probes && fake->discoveries == discoveries &&
            app.snapshot().remembered && fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0,
            "notification-only reacquisition preserves persistent eligibility without issuing physical probes, proof requests or credentials");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == probes + 1; });
        check(fake->captures >= captures + 9 && fake->probes == probes + 1 && app.snapshot().remembered &&
            app.snapshot().epoch == epoch && fake->evidence().fake_auth_submissions == 0,
            "the original due callback still captures the physical attachment and performs its next read-only monitoring round");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        check(app.trust(), "reacquisition fixture starts with explicit session trust independent of its saved memory");
        fake->candidate_available = false; fake->tunnel = true;
        app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        const auto unavailable_epoch = app.snapshot().epoch;
        check(!app.snapshot().trusted && !app.snapshot().remembered && !app.snapshot().authentication_route &&
            app.snapshot().memory_count == 1 && fake->files[L"network-memory.json"] == memory,
            "temporary physical identification failure revokes session approval but keeps the durable remembered record intact");
        for (int round = 1; round <= 2; ++round) {
            const int captures = fake->captures;
            app.on_timer(kTimerAuto);
            networkrecoveryscheduleruntime::finish(app, [&] { return fake->captures > captures && app.snapshot().status == EngineStatus::unconfirmed; });
            const auto scheduled = app.next_check();
            for (int signal = 0; signal < 3; ++signal) {
                app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
                check(app.next_check() == scheduled && app.snapshot().epoch == unavailable_epoch,
                    "repeated insufficient snapshots keep the existing retry deadline instead of indefinitely restarting it");
            }
        }
        check(fake->probes == 0 && fake->discoveries == 0 && fake->bindings == 0 && fake->evidence().fake_auth_submissions == 0 &&
            fake->files[L"network-memory.json"] == memory,
            "an unavailable physical candidate is never cached or used for a probe, proof request or credential submission");
        fake->candidate_available = true; app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        check(app.snapshot().epoch != unavailable_epoch && app.snapshot().remembered && !app.snapshot().trusted &&
            app.snapshot().authentication_route && app.next_check() && *app.next_check() <= AppController::wall_now() + 1,
            "a newly stable physical binding rematches saved evidence and schedules reacquisition without restoring session trust");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1 && fake->returned == 1; });
        check(app.snapshot().online && app.snapshot().required_campus_confirmed && !app.snapshot().trusted &&
            fake->discoveries == 1 && fake->evidence().fake_auth_submissions == 0 && fake->files[L"network-memory.json"] == memory,
            "healthy reacquisition reads fixed probes and independently verifies campus service without requiring the user to remember again");
        fake->candidate_available = false; app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        fake->candidate_available = true; fake->online = false;
        app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        const int probes = fake->probes, discoveries = fake->discoveries;
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == probes + 1; });
        const auto confirmation_due = app.next_check();
        check(app.snapshot().status == EngineStatus::confirming && fake->discoveries == discoveries &&
            fake->evidence().fake_auth_submissions == 0, "restored memory does not bypass the first physical failure confirmation");
        for (int signal = 0; signal < 6; ++signal) {
            app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
            check(app.next_check() == confirmation_due && app.snapshot().remembered,
                "TUN notification bursts cannot keep moving the one-second second-observation deadline");
        }
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->evidence().fake_auth_submissions == 1 && fake->probes >= probes + 3; });
        check(app.snapshot().online && fake->discoveries == discoveries + 1 && fake->evidence().fake_auth_submissions == 1 &&
            fake->fake_credentials && fake->selected_area == CampusArea::teaching && fake->files[L"network-memory.json"] == memory,
            "two fresh failed physical observations permit exactly one freshly verified fake teaching submission and its successful recheck");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_wifi_calls == 0,
            "physical identity reacquisition and recovery use isolated services, fake credentials and temporary persistence only");
    }
    for (const int reason : {-1, 0, 1, 2, 3}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->online = false;
        const std::string holds = reason < 0 ? "" : "{\"reason\":" + std::to_string(reason) + ",\"origin\":2}";
        fake->files[L"recovery.json"] = "{\"schema\":1,\"generation\":1,\"notBefore\":" +
            std::to_string(static_cast<long long>(AppController::wall_now() + 90)) + ",\"holds\":[" + holds + "]}";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1; });
        const auto deadline = app.next_check();
        const auto before = app.snapshot();
        const auto journal = fake->files[L"recovery.json"];
        fake->constrained = true;
        for (int signal = 0; signal < 4; ++signal) {
            fake->tunnel = !fake->tunnel; app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        }
        check(deadline && app.next_check() == deadline && proxyassistantruntime::same_holds(before.holds, app.snapshot().holds) &&
            before.epoch == app.snapshot().epoch && fake->files[L"recovery.json"] == journal && fake->evidence().fake_auth_submissions == 0,
            "same-attachment notifications preserve the existing deadline, cooldown and user/security/uncertain/credential holds");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 2; });
        check(proxyassistantruntime::same_holds(before.holds, app.snapshot().holds) && app.snapshot().cooldown_remaining &&
            fake->files[L"recovery.json"] == journal && fake->evidence().fake_auth_submissions == 0,
            "a retained timer never grants authentication or clears a hold before the engine's independent protected deadline");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->constrained = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1 && fake->returned == 1; });
        const auto deadline = app.next_check();
        check(deadline && *deadline > AppController::wall_now() + 110,
            "independent-verification notification fixture has a genuine long existing monitoring deadline");
        fake->constrained = false; app.refresh_environment(); fake->block = true;
        const int completed = fake->returned;
        check(app.verify_campus_service(), "independent verification reserves the long timer before its notification race");
        wifiruntime::wait_for([&] { return fake->discoveries == completed + 1; });
        for (int signal = 0; signal < 3; ++signal) {
            fake->tunnel = !fake->tunnel; app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
            check(!app.next_check() && app.snapshot().verifying_campus,
                "network notifications cannot recreate a monitoring timer suspended by independent campus verification");
        }
        app.cancel(); fake->released = true;
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->returned == completed + 1; });
        check(deadline && app.next_check() && *app.next_check() >= *deadline && !app.snapshot().required_campus_confirmed &&
            fake->probes == 1 && fake->evidence().fake_auth_submissions == 0,
            "cancelled verification restores the full old deadline after virtual notifications and rejects its late success");
        app.on_suspend();
        const int probes = fake->probes, discoveries = fake->discoveries;
        app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        check(app.snapshot().sleeping && !app.next_check() && !app.snapshot().trusted && fake->probes == probes &&
            fake->discoveries == discoveries && fake->evidence().fake_auth_submissions == 0,
            "network notifications and their UI completion cannot rearm automatic monitoring across a sleep boundary");
        app.shutdown();
    }
}
